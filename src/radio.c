#include "radio.h"
#include "apps/apps.h"
#include "board.h"
#include "dcs.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/battery.h"
#include "driver/bk1080.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/lfs.h"
#include "driver/si473x.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/bands.h"
#include "helper/fsk2.h"
#include "helper/lootlist.h"
#include "helper/measurements.h"
#include "helper/storage.h"
#include "inc/band.h"
#include "inc/channel.h"
#include "inc/common.h"
#include "inc/vfo.h"
#include "misc.h"
#include "radio_switch.h"
#include "settings.h"
#include <stdint.h>
#include <string.h>

#define RADIO_SAVE_DELAY_MS 2000

// #define DEBUG_PARAMS 1

bool gShowAllRSSI = false;
bool gMonitorMode = false;

Band gCurrentBand;

RadioState *gRadioState;
ExtendedVFOContext *vfo;
VFOContext *ctx;

const char *TX_POWER_NAMES[4] = {"ULow", "Low", "Mid", "High"};
const char *TX_OFFSET_NAMES[4] = {"None", "+", "-", "Freq"};

const char *RADIO_NAMES[3] = {
    [RADIO_BK4819] = "BK4819",
    [RADIO_BK1080] = "BK1080",
    [RADIO_SI4732] = "SI4732",
};

const char *FILTER_NAMES[4] = {
    [FILTER_VHF] = "VHF",
    [FILTER_UHF] = "UHF",
    [FILTER_OFF] = "Off",
    [FILTER_AUTO] = "Auto",
};

// Дескриптор параметра: имя + диапазон для RADIO_AdjustParam.
// UINT32_MAX в max_val означает динамический диапазон (зависит от
// радио/диапазона). Для read-only параметров min_val == max_val == 0.
typedef struct {
  const char *name;
  uint32_t min_val;
  uint32_t max_val;
} ParamDesc;

static const ParamDesc PARAM_DESC[PARAM_COUNT] = {
    [PARAM_FREQUENCY] = {"f", 0, UINT32_MAX}, // диапазон из band
    [PARAM_STEP] = {"Step", 0, STEP_COUNT},
    [PARAM_POWER] = {"Power", 0, 4},
    [PARAM_TX_OFFSET] = {"TX offset", 0, UINT32_MAX}, // диапазон из band
    [PARAM_TX_OFFSET_DIR] = {"TX offset dir", 0, OFFSET_FREQ + 1},
    [PARAM_MODULATION] = {"Mod", 0, UINT32_MAX}, // index-based
    [PARAM_SQUELCH_VALUE] = {"SQ", 0, 11},
    [PARAM_SQUELCH_TYPE] = {"SQ type", 0, 4},
    [PARAM_VOLUME] = {"Volume", 0, 100},
    [PARAM_GAIN] = {"ATT", 0, UINT32_MAX},     // зависит от радио
    [PARAM_BANDWIDTH] = {"BW", 0, UINT32_MAX}, // index-based
    [PARAM_TX_STATE] = {"TX state", 0, 0},     // read-only
    [PARAM_RADIO] = {"Radio", 0, 3},
    [PARAM_RX_CODE] = {"RX code", 0, UINT32_MAX},
    [PARAM_TX_CODE] = {"TX code", 0, UINT32_MAX},
    [PARAM_RSSI] = {"RSSI", 0, 0},     // read-only
    [PARAM_GLITCH] = {"Glitch", 0, 0}, // read-only
    [PARAM_NOISE] = {"Noise", 0, 0},   // read-only
    [PARAM_SNR] = {"SNR", 0, 0},       // read-only
    [PARAM_PRECISE_F_CHANGE] = {"Precise f", 0, 1},
    [PARAM_TX_POWER] = {"TX Power", 0, 255},
    [PARAM_TX_POWER_AMPLIFIER] = {"TX PA", 0, 2},
    [PARAM_TX_FREQUENCY] = {"TX f", 0, UINT32_MAX}, // диапазон из band
    [PARAM_TX_FREQUENCY_FACT] = {"TX f fact", 0, 0}, // computed, read-only
    [PARAM_AFC] = {"AFC", 0, 8},
    [PARAM_AFC_SPD] = {"AFC SPD", 0, 64},
    [PARAM_AF_RX_300] = {"RX 300", 0, 9},
    [PARAM_AF_RX_3K] = {"RX 3K", 0, 9},
    [PARAM_AF_TX_300] = {"TX 300", 0, 9},
    [PARAM_AF_TX_3K] = {"TX 3K", 0, 9},
    [PARAM_DEV] = {"DEV", 0, 2550},
    [PARAM_MIC] = {"MIC", 0, 16},
    [PARAM_AGC] = {"AGC", 0, 127},
    [PARAM_XTAL] = {"XTAL", 0, XTAL_3_38_4M + 1},
    [PARAM_SCRAMBLER] = {"SCR", 0, 10},
    [PARAM_FILTER] = {"Filter", 0, FILTER_AUTO + 1},
    [PARAM_UPCONVERTER] = {"Upconv", 0, UINT32_MAX},
};

// Удобный алиас для совместимости с кодом, который обращается к PARAM_NAMES
#define PARAM_NAMES(p) (PARAM_DESC[p].name)

const char *RADIO_GetParamName(ParamType p) { return PARAM_DESC[p].name; }

const char *TX_STATE_NAMES[7] = {
    [TX_UNKNOWN] = "TX Off",              //
    [TX_ON] = "TX On",                    //
    [TX_VOL_HIGH] = "CHARGING",           //
    [TX_BAT_LOW] = "BAT LOW",             //
    [TX_DISABLED] = "DISABLED",           //
    [TX_DISABLED_UPCONVERTER] = "UPCONV", //
    [TX_POW_OVERDRIVE] = "HIGH POW",      //
};

const char *MOD_NAMES_BK4819[8] = {
    [MOD_FM] = "FM",   //
    [MOD_AM] = "AM",   //
    [MOD_LSB] = "DSB", //
    [MOD_USB] = "DSB", //
    [MOD_BYP] = "BYP", //
    [MOD_RAW] = "RAW", //
    [MOD_WFM] = "WFM", //
};

const char *MOD_NAMES_SI47XX[8] = {
    [SI47XX_AM] = "AM",
    [SI47XX_FM] = "FM",
    [SI47XX_LSB] = "LSB",
    [SI47XX_USB] = "USB",
};

const char *BW_NAMES_BK4819[10] = {
    [BK4819_FILTER_BW_6k] = "U6K",   //
    [BK4819_FILTER_BW_7k] = "U7K",   //
    [BK4819_FILTER_BW_9k] = "N9k",   //
    [BK4819_FILTER_BW_10k] = "N10k", //
    [BK4819_FILTER_BW_12k] = "W12k", //
    [BK4819_FILTER_BW_14k] = "W14k", //
    [BK4819_FILTER_BW_17k] = "W17k", //
    [BK4819_FILTER_BW_20k] = "W20k", //
    [BK4819_FILTER_BW_23k] = "W23k", //
    [BK4819_FILTER_BW_26k] = "W26k", //
};

const char *BW_NAMES_SI47XX[7] = {
    [SI47XX_BW_1_8_kHz] = "1.8k", //
    [SI47XX_BW_1_kHz] = "1k",     //
    [SI47XX_BW_2_kHz] = "2k",     //
    [SI47XX_BW_2_5_kHz] = "2.5k", //
    [SI47XX_BW_3_kHz] = "3k",     //
    [SI47XX_BW_4_kHz] = "4k",     //
    [SI47XX_BW_6_kHz] = "6k",     //
};

const char *BW_NAMES_SI47XX_SSB[6] = {
    [SI47XX_SSB_BW_0_5_kHz] = "0.5k", //
    [SI47XX_SSB_BW_1_0_kHz] = "1.0k", //
    [SI47XX_SSB_BW_1_2_kHz] = "1.2k", //
    [SI47XX_SSB_BW_2_2_kHz] = "2.2k", //
    [SI47XX_SSB_BW_3_kHz] = "3k",     //
    [SI47XX_SSB_BW_4_kHz] = "4k",     //
};

const char *FLT_BOUND_NAMES[2] = {"240MHz", "280MHz"};

const char *SQ_TYPE_NAMES[4] = {"RNG", "RG", "RN", "R"};

const uint16_t StepFrequencyTable[15] = {
    2,   5,   50,  100,

    250, 500, 625, 833, 900, 1000, 1250, 2500, 5000, 10000, 50000,
};

// Диапазоны для BK4819
static const FreqBand bk4819_bands[] = {
    {
        .min_freq = BK4819_F_MIN,
        .max_freq = BK4819_F_MAX,
        .num_available_mods = 6,
        .num_available_bandwidths = 10,
        .available_mods = {MOD_FM, MOD_AM, MOD_LSB, MOD_BYP, MOD_RAW, MOD_WFM},
        .available_bandwidths =
            {
                BK4819_FILTER_BW_6k,  //  "U 6K"
                BK4819_FILTER_BW_7k,  //  "U 7K",
                BK4819_FILTER_BW_9k,  //  "N 9k",
                BK4819_FILTER_BW_10k, //  "N 10k",
                BK4819_FILTER_BW_12k, //  "W 12k",
                BK4819_FILTER_BW_14k, //  "W 14k",
                BK4819_FILTER_BW_17k, //  "W 17k",
                BK4819_FILTER_BW_20k, //  "W 20k",
                BK4819_FILTER_BW_23k, //  "W 23k",
                BK4819_FILTER_BW_26k, //  "W 26k",
            },
    },
};

// Диапазоны для SI4732
static const FreqBand si4732_bands[] = {
    {
        .min_freq = SI47XX_F_MIN,
        .max_freq = SI47XX_F_MAX,
        .num_available_mods = 3,
        .num_available_bandwidths = 7,
        .available_mods = {SI47XX_AM, SI47XX_LSB, SI47XX_USB},
        .available_bandwidths =
            {
                SI47XX_BW_1_kHz,
                SI47XX_BW_1_8_kHz,
                SI47XX_BW_2_kHz,
                SI47XX_BW_2_5_kHz,
                SI47XX_BW_3_kHz,
                SI47XX_BW_4_kHz,
                SI47XX_BW_6_kHz,
            },
    },
    {
        .min_freq = SI47XX_F_MIN,
        .max_freq = SI47XX_F_MAX,
        .num_available_mods = 3,
        .num_available_bandwidths = 6,
        .available_mods = {SI47XX_AM, SI47XX_LSB, SI47XX_USB},
        .available_bandwidths =
            {
                SI47XX_SSB_BW_0_5_kHz,
                SI47XX_SSB_BW_1_0_kHz,
                SI47XX_SSB_BW_1_2_kHz,
                SI47XX_SSB_BW_2_2_kHz,
                SI47XX_SSB_BW_3_kHz,
                SI47XX_SSB_BW_4_kHz,
            },
    },
    {
        .min_freq = SI47XX_FM_F_MIN,
        .max_freq = SI47XX_FM_F_MAX,
        .num_available_mods = 1,
        .num_available_bandwidths = 0,
        .available_mods = {SI47XX_FM},
        .available_bandwidths = {},
    },
};

char vfosDirName[16];
char vfosFileName[32];

static void initVfoFile() {
  snprintf(vfosDirName, 16, "/%s", apps[gCurrentApp].name);
  snprintf(vfosFileName, 32, "%s/vfos.vfo", vfosDirName);
  Log("[RADIO] INIT VFOs FILE %s", vfosFileName);

  struct lfs_info info;
  if (lfs_stat(&gLfs, vfosDirName, &info) < 0) {
    lfs_mkdir(&gLfs, vfosDirName);
  }

  if (!lfs_file_exists(vfosFileName)) {
    STORAGE_INIT(vfosFileName, VFO, MAX_VFOS);
    VFO vfos[] = {
        {
            .name = "VFO A",
            .rxF = 43392500,
            .bw = BK4819_FILTER_BW_12k,
            .step = STEP_25_0kHz,
            .squelch.value = 4,
            .mic = 8,
            .deviation = 126,
            .upconverter = 0,
        },
        {
            .name = "VFO B",
            .rxF = 17230000,
            .bw = BK4819_FILTER_BW_12k,
            .step = STEP_25_0kHz,
            .squelch.value = 4,
            .mic = 8,
            .deviation = 126,
            .upconverter = 0,
        },
        {
            .name = "VFO C",
            .rxF = 25355000,
            .bw = BK4819_FILTER_BW_12k,
            .step = STEP_25_0kHz,
            .squelch.value = 4,
            .mic = 8,
            .deviation = 126,
            .upconverter = 0,
        },
        {
            .name = "VFO D",
            .rxF = 14550000,
            .bw = BK4819_FILTER_BW_12k,
            .step = STEP_25_0kHz,
            .squelch.value = 4,
            .mic = 8,
            .deviation = 126,
            .upconverter = 0,
        },
    };
    for (uint8_t i = 0; i < MAX_VFOS; ++i) {
      STORAGE_SAVE(vfosFileName, i, &vfos[i]);
    }
  }
}

static void saveVfo(uint8_t i, VFO *vfo) {
  // vfosDirName и vfosFileName должны быть инициализированы через initVfoFile()
  // (она вызывается в RADIO_LoadVFOs перед любым saveVfo)
  struct lfs_info info;
  if (lfs_stat(&gLfs, vfosDirName, &info) < 0) {
    lfs_mkdir(&gLfs, vfosDirName);
  }

  Log("[RADIO] SAVE VFO %u", i);
  STORAGE_SAVE(vfosFileName, i, vfo);
}

static void loadVfo(uint8_t i, VFO *vfo) {
  Log("[RADIO] LOAD VFO %u", i);
  STORAGE_LOAD(vfosFileName, i, vfo);
}

static bool RADIO_HasSi() { return BK1080_ReadRegister(1) != 0x1080; }

static uint32_t getRealTxFreq(const VFOContext *ctx) {
  switch (ctx->tx_state.offsetDirection) {
  case OFFSET_PLUS:
    return ctx->frequency + ctx->tx_state.frequency;
  case OFFSET_MINUS:
    return ctx->frequency - ctx->tx_state.frequency;
  case OFFSET_FREQ:
    return ctx->tx_state.frequency;
  default:
    break;
  }
  return ctx->frequency;
}

static void enableCxCSS(VFOContext *ctx) {
  switch (ctx->tx_state.code.type) {
  case CODE_TYPE_CONTINUOUS_TONE:
    BK4819_SetCTCSSFrequency(CTCSS_Options[ctx->tx_state.code.value]);
    break;
  case CODE_TYPE_DIGITAL:
  case CODE_TYPE_REVERSE_DIGITAL:
    BK4819_SetCDCSSCodeWord(DCS_GetGolayCodeWord(ctx->tx_state.code.type,
                                                 ctx->tx_state.code.value));
    break;
  default:
    BK4819_ExitSubAu();
    break;
  }
}

#include "./ui/toast.h"

void RADIO_SetupToneDetection(VFOContext *ctx) {
  // BK4819_WriteRegister(BK4819_REG_7E, 0x302E); // DC flt BW 0=BYP

  uint16_t InterruptMask = BK4819_REG_3F_CxCSS_TAIL;

  InterruptMask |= BK4819_REG_3F_FSK_RX_SYNC |
                   BK4819_REG_3F_FSK_FIFO_ALMOST_FULL |
                   BK4819_REG_3F_FSK_RX_FINISHED;

  // InterruptMask |= BK4819_REG_3F_SQUELCH_LOST | BK4819_REG_3F_SQUELCH_FOUND;

  if (gSettings.dtmfdecode) {
    BK4819_EnableDTMF();
    InterruptMask |= BK4819_REG_3F_DTMF_5TONE_FOUND;
  } else {
    BK4819_DisableDTMF();
  }
  switch (ctx->code.type) {
  case CODE_TYPE_DIGITAL:
  case CODE_TYPE_REVERSE_DIGITAL:
    // Log("DCS on");
    InterruptMask |= BK4819_REG_3F_CDCSS_FOUND | BK4819_REG_3F_CDCSS_LOST;
    BK4819_SetCDCSSCodeWord(
        DCS_GetGolayCodeWord(ctx->code.type, ctx->code.value));
    // TOAST_Push("CD ON");
    break;
  case CODE_TYPE_CONTINUOUS_TONE:
    // Log("CTCSS on");
    InterruptMask |= BK4819_REG_3F_CTCSS_FOUND | BK4819_REG_3F_CTCSS_LOST;
    BK4819_SetCTCSSFrequency(CTCSS_Options[ctx->code.value]);
    // TOAST_Push("CT ON");

    break;
  default:
    // Log("STE on");
    BK4819_SetCTCSSFrequency(670);
    BK4819_SetTailDetection(550);
    break;
  }
  BK4819_WriteRegister(BK4819_REG_3F, InterruptMask);

  RF_EnterFsk();
}

static void sendEOT() {
  BK4819_ExitSubAu();
  switch (gSettings.roger) {
  case 1:
    BK4819_PlayRogerTiny();
    break;
  default:
    break;
  }
  if (gSettings.ste) {
    SYSTICK_DelayMs(50);
    BK4819_GenTail(4);
    BK4819_WriteRegister(BK4819_REG_51, 0x9033);
    SYSTICK_DelayMs(200);
  }
  BK4819_ExitSubAu();
}

static TXStatus checkTX(VFOContext *ctx) {
  if (ctx->upconverter) {
    return TX_DISABLED_UPCONVERTER;
  }

  if (ctx->radio_type != RADIO_BK4819) {
    return TX_DISABLED;
  }

  /* Band txBand = BANDS_ByFrequency(txF);

  if (!txBand.allowTx && !(RADIO_IsChMode() && radio->allowTx)) {
    return TX_DISABLED;
  } */

  if (gBatteryPercent == 0) {
    return TX_BAT_LOW;
  }
  if (gChargingWithTypeC || gBatteryVoltage > 880) {
    return TX_VOL_HIGH;
  }
  return TX_ON;
}

void RADIO_SwitchAudioToVFO(RadioState *state, uint8_t vfo_index) {
  if (vfo_index >= state->num_vfos)
    return;
  Log("[RADIO] SW AUD to VFO %u", vfo_index);
  const ExtendedVFOContext *ev = &state->vfos[vfo_index];
  RXSW_SwitchTo(&state->rx_switch, &ev->context, ev->is_open);
  BOARD_ToggleGreen(ev->is_open);
  if (ev->is_open) {
    if (gSettings.backlightOnSquelch != BL_SQL_OFF)
      BACKLIGHT_TurnOn();
  } else {
    if (gSettings.backlightOnSquelch == BL_SQL_OPEN)
      BACKLIGHT_TurnOff();
  }
}

static bool setParamBK4819(VFOContext *ctx, ParamType p) {
  switch (p) {
  case PARAM_GAIN:
    BK4819_SetAGC(ctx->modulation != MOD_AM, ctx->gain);
    return true;
  case PARAM_BANDWIDTH:
    BK4819_SetFilterBandwidth(ctx->bandwidth);
    return true;
  case PARAM_SQUELCH_VALUE:
    BK4819_Squelch(ctx->squelch.value, ctx->frequency, gSettings.sqlOpenTime,
                   gSettings.sqlCloseTime);
    return true;
  case PARAM_SQUELCH_TYPE:
    BK4819_SquelchType(ctx->squelch.type);
    return true;
  case PARAM_MODULATION:
    BK4819_SetModulation(ctx->modulation);
    // SetModulation безусловно снимает AFC_DIS для не-SSB режимов (REG_73
    // бит 4 — тот же бит, что использует SetAFC), поэтому включает AFC
    // заново независимо от ctx->afc. Восстанавливаем реальное состояние.
    BK4819_SetAFC(RADIO_IsSSB(ctx) ? 0 : ctx->afc);
    return true;
  case PARAM_FREQUENCY:
    if (ctx->filter == FILTER_AUTO) {
      Filter filter = (ctx->frequency < SETTINGS_GetFilterBound()) ? FILTER_VHF
                                                                   : FILTER_UHF;
      BK4819_SelectFilterEx(filter);
    }
    BK4819_TuneTo(ctx->frequency, ctx->preciseFChange);
    return true;
  case PARAM_FREQUENCY_FACT:
    // Read-only computed value, nothing to set
    return true;
  case PARAM_AFC:
    BK4819_SetAFC(ctx->afc);
    return true;
  case PARAM_AFC_SPD:
    BK4819_SetAFCSpeed(ctx->afc_speed);
    return true;
  case PARAM_AF_RX_300:
    RADIO_ApplyAFResponse(false, false);
    return true;
  case PARAM_AF_RX_3K:
    RADIO_ApplyAFResponse(false, true);
    return true;
  case PARAM_AF_TX_300:
    RADIO_ApplyAFResponse(true, false);
    return true;
  case PARAM_AF_TX_3K:
    RADIO_ApplyAFResponse(true, true);
    return true;
  case PARAM_XTAL:
    BK4819_XtalSet(ctx->xtal);
    return true;
  case PARAM_SCRAMBLER:
    BK4819_SetScrambler(ctx->scrambler);
    return true;
  case PARAM_FILTER: {
    Filter filter = ctx->filter;
    if (ctx->filter == FILTER_AUTO) {
      filter = (ctx->frequency < SETTINGS_GetFilterBound()) ? FILTER_VHF
                                                            : FILTER_UHF;
    }
    BK4819_SelectFilterEx(filter);
  }
    return true;
  case PARAM_MIC:
    BK4819_SetRegValue(RS_MIC, ctx->mic);
    return true;
  case PARAM_DEV:
    BK4819_SetRegValue(RS_DEV, ctx->dev);
    return true;
  case PARAM_AGC:
    BK4819_SetRegValue(RS_AGC, ctx->agc);
    return true;
  case PARAM_UPCONVERTER:
    // Upconverter affects frequency calculation, requires retune
    return true;
  case PARAM_VOLUME:
    return true;
  case PARAM_RADIO:
  case PARAM_PRECISE_F_CHANGE:
  case PARAM_STEP:
  case PARAM_POWER:
  case PARAM_TX_OFFSET:
  case PARAM_TX_OFFSET_DIR:
  case PARAM_TX_STATE:
  case PARAM_TX_FREQUENCY:
  case PARAM_TX_FREQUENCY_FACT:
  case PARAM_TX_POWER:
  case PARAM_TX_POWER_AMPLIFIER:
  case PARAM_RX_CODE:
  case PARAM_TX_CODE:
  case PARAM_RSSI:
  case PARAM_NOISE:
  case PARAM_GLITCH:
  case PARAM_SNR:
  case PARAM_COUNT:
    break;
  }
  return false;
}

static bool setParamSI4732(VFOContext *ctx, ParamType p) {
  switch (p) {
  case PARAM_FREQUENCY:
    SI47XX_TuneTo(ctx->frequency);
    return true;
  case PARAM_FREQUENCY_FACT:
    // Read-only computed value, nothing to set
    return true;
  case PARAM_MODULATION:
    SI47XX_SwitchMode((SI47XX_MODE)ctx->modulation);
    return true;
  case PARAM_GAIN:
    SI47XX_SetAutomaticGainControl(ctx->gain == 0,
                                   ctx->gain == 0 ? 0 : ctx->gain);
    return true;
  case PARAM_BANDWIDTH:
    if (RADIO_IsSSB(ctx)) {
      SI47XX_SetSsbBandwidth(ctx->bandwidth);
    } else {
      SI47XX_SetBandwidth(ctx->bandwidth, true);
    }
    return true;
  case PARAM_VOLUME:
    SI47XX_SetVolume(ConvertDomain(ctx->volume, 0, 100, 0, 63));
    return true;
  case PARAM_RADIO:
  case PARAM_PRECISE_F_CHANGE:
  case PARAM_STEP:
  case PARAM_POWER:
  case PARAM_SQUELCH_TYPE:
  case PARAM_SQUELCH_VALUE:
  case PARAM_TX_OFFSET:
  case PARAM_TX_OFFSET_DIR:
  case PARAM_TX_STATE:
  case PARAM_TX_FREQUENCY:
  case PARAM_TX_FREQUENCY_FACT:
  case PARAM_TX_POWER:
  case PARAM_TX_POWER_AMPLIFIER:
  case PARAM_RX_CODE:
  case PARAM_TX_CODE:
  case PARAM_AFC:
  case PARAM_AFC_SPD:
  case PARAM_AF_RX_300:
  case PARAM_AF_RX_3K:
  case PARAM_AF_TX_300:
  case PARAM_AF_TX_3K:
  case PARAM_DEV:
  case PARAM_MIC:
  case PARAM_AGC:
  case PARAM_XTAL:
  case PARAM_SCRAMBLER:
  case PARAM_FILTER:
  case PARAM_UPCONVERTER:
  case PARAM_RSSI:
  case PARAM_NOISE:
  case PARAM_GLITCH:
  case PARAM_SNR:
  case PARAM_COUNT:
    break;
  }
  return false;
}

static bool setParamBK1080(VFOContext *ctx, ParamType p) {
  if (p == PARAM_FREQUENCY) {
    BK4819_SelectFilter(ctx->frequency);
    BK1080_SetFrequency(ctx->frequency);
    return true;
  }
  if (p == PARAM_FREQUENCY_FACT) {
    // Read-only computed value, nothing to set
    return true;
  }
  // AF parameters are BK4819-specific
  if (p == PARAM_AF_RX_300 || p == PARAM_AF_RX_3K || p == PARAM_AF_TX_300 ||
      p == PARAM_AF_TX_3K || p == PARAM_UPCONVERTER) {
    return true;
  }
  return false;
}

uint16_t RADIO_GetRSSI(const VFOContext *ctx) {
  switch (ctx->radio_type) {
  case RADIO_BK4819:
    return BK4819_GetRSSI();
  case RADIO_BK1080:
    return gShowAllRSSI ? BK1080_GetRSSI() : 0;
  case RADIO_SI4732:
    if (gShowAllRSSI) {
      RSQ_GET();
      return rsqStatus.resp.RSSI;
    }
    return 0;
  default:
    return 0;
  }
}

uint8_t RADIO_GetSNR(const VFOContext *ctx) {
  switch (ctx->radio_type) {
  case RADIO_BK4819:
    return ConvertDomain(BK4819_GetSNR(), 24, 170, 0, 30);
  case RADIO_BK1080:
    return gShowAllRSSI ? BK1080_GetSNR() : 0;
  case RADIO_SI4732:
    if (gShowAllRSSI) {
      RSQ_GET();
      return rsqStatus.resp.SNR;
    }
    return 0;
  default:
    return 0;
  }
}

uint8_t RADIO_GetNoise(const VFOContext *ctx) {
  return ctx->radio_type == RADIO_BK4819 ? BK4819_GetNoise() : 0;
}

uint8_t RADIO_GetGlitch(const VFOContext *ctx) {
  return ctx->radio_type == RADIO_BK4819 ? BK4819_GetGlitch() : 0;
}

static void updateContext() {
  vfo = RADIO_GetCurrentVFO(gRadioState);
  ctx = &vfo->context;
}

static void RADIO_ApplyCorrections(VFOContext *ctx, bool save_to_eeprom) {
  const FreqBand *band = ctx->current_band;
  if (!band)
    return; // Нет band — ничего не корректируем

  // Корректировка частоты (если вне диапазона)
  if (!RADIO_IsParamValid(ctx, PARAM_FREQUENCY, ctx->frequency)) {
    LogC(LOG_C_YELLOW,
         "[RADIO] CORRECT: Frequency %u out of band, adjusting to nearest "
         "boundary",
         ctx->frequency);

    // пробуем подобрать приёмник сперва
    if (ctx->radio_type == RADIO_BK4819) {
      if (RADIO_HasSi()) {
        for (uint8_t i = 0; i < ARRAY_SIZE(si4732_bands); ++i) {
          if (ctx->frequency >= si4732_bands[i].min_freq &&
              ctx->frequency <= si4732_bands[i].max_freq) {
            band = &si4732_bands[i];
            // ctx->radio_type = RADIO_SI4732;
            RADIO_SetParam(ctx, PARAM_RADIO, RADIO_SI4732, false);
          }
        }
      } else if (ctx->frequency >= BK1080_F_MIN &&
                 ctx->frequency <= BK1080_F_MAX) {
        // ctx->radio_type = RADIO_BK1080;
        RADIO_SetParam(ctx, PARAM_RADIO, RADIO_BK1080, false);
      }
    } else {
      // ctx->radio_type = RADIO_BK4819;
      RADIO_SetParam(ctx, PARAM_RADIO, RADIO_BK4819, false);
    }

    if (ctx->frequency < band->min_freq) {
      LogC(LOG_C_BRIGHT_YELLOW, "[RADIO] CORRECT F=%u ~ %u (%u..%u)",
           ctx->frequency, band->min_freq, band->max_freq);
      ctx->frequency = band->min_freq;
    } else if (ctx->frequency > band->max_freq) {
      LogC(LOG_C_BRIGHT_YELLOW, "[RADIO] CORRECT F=%u ~ %u (%u..%u)",
           ctx->frequency, band->min_freq, band->max_freq);
      ctx->frequency = band->max_freq;
    }
    ctx->dirty[PARAM_FREQUENCY] = true; // Помечаем как dirty для применения
    if (save_to_eeprom) {
      ctx->save_to_eeprom = true;
      ctx->last_save_time = Now();
    }
  }

  // Корректировка модуляции (если не доступна)
  if (!RADIO_IsParamValid(ctx, PARAM_MODULATION, ctx->modulation)) {
    if (band->num_available_mods > 0) {
      uint32_t default_mod = band->available_mods[0];
      LogC(LOG_C_YELLOW,
           "[RADIO] CORRECT: Modulation %u invalid for band, setting to %u "
           "(%s)",
           ctx->modulation, default_mod,
           RADIO_GetParamValueString(
               ctx, PARAM_MODULATION)); // Используем новую мод для строки
      ctx->modulation = default_mod;
      ctx->dirty[PARAM_MODULATION] = true;
      if (save_to_eeprom) {
        ctx->save_to_eeprom = true;
        ctx->last_save_time = Now();
      }
    }
  }

  // Корректировка bandwidth (если не доступна)
  if (!RADIO_IsParamValid(ctx, PARAM_BANDWIDTH, ctx->bandwidth)) {
    if (band->num_available_bandwidths > 0) {
      uint32_t default_bw = band->available_bandwidths[0];
      LogC(LOG_C_YELLOW,
           "[RADIO] CORRECT: Bandwidth %u invalid for band, setting to %u "
           "(%s)",
           ctx->bandwidth, default_bw,
           RADIO_GetParamValueString(ctx, PARAM_BANDWIDTH));
      ctx->bandwidth = default_bw;
      ctx->dirty[PARAM_BANDWIDTH] = true;
      if (save_to_eeprom) {
        ctx->save_to_eeprom = true;
        ctx->last_save_time = Now();
      }
    }
  }

  // Можно добавить корректировки для других params, если нужно (e.g., gain,
  // step) Например, для gain: if (!RADIO_IsParamValid(ctx, PARAM_GAIN,
  // ctx->gain)) { ... set default ... }
}

static void RADIO_UpdateCurrentBand(VFOContext *ctx) {
  const FreqBand *band = NULL;

  switch (ctx->radio_type) {
  case RADIO_BK4819:
    band = &bk4819_bands[0];
    break;
  case RADIO_SI4732:
    // Выбираем диапазон на основе частоты и модуляции
    if (ctx->frequency >= SI47XX_FM_F_MIN &&
        ctx->frequency <= SI47XX_FM_F_MAX) {
      band = &si4732_bands[2]; // FM
    } else if ((SI47XX_MODE)ctx->modulation == SI47XX_LSB ||
               (SI47XX_MODE)ctx->modulation == SI47XX_USB) {
      band = &si4732_bands[1]; // SSB
    } else {
      band = &si4732_bands[0]; // AM
    }
    break;
  case RADIO_BK1080:
    band = &bk4819_bands[0]; // TODO: добавить свой диапазон
    break;
  default:
    break;
  }

  ctx->current_band = band;

  // === Автоматически пересчитываем индексы ===
  ctx->modulation_index = 0;
  for (uint8_t i = 0; i < band->num_available_mods; i++) {
    if (band->available_mods[i] == ctx->modulation) {
      ctx->modulation_index = i;
      break;
    }
  }

  ctx->bandwidth_index = 0;
  for (uint8_t i = 0; i < band->num_available_bandwidths; i++) {
    if (band->available_bandwidths[i] == ctx->bandwidth) {
      ctx->bandwidth_index = i;
      break;
    }
  }

  // Если не нашли — принудительно установим 0 (первый в списке)
  if (ctx->modulation_index >= band->num_available_mods &&
      band->num_available_mods > 0) {
    ctx->modulation_index = 0;
    ctx->modulation = band->available_mods[0];
    ctx->dirty[PARAM_MODULATION] = true;
  }
  if (ctx->bandwidth_index >= band->num_available_bandwidths &&
      band->num_available_bandwidths > 0) {
    ctx->bandwidth_index = 0;
    ctx->bandwidth = band->available_bandwidths[0];
    ctx->dirty[PARAM_BANDWIDTH] = true;
  }
}

// Инициализация VFO
void RADIO_Init(VFOContext *ctx, Radio radio_type) {
  memset(ctx, 0, sizeof(VFOContext));
  ctx->radio_type = radio_type;

  // Установка диапазона по умолчанию
  switch (radio_type) {
  case RADIO_BK4819:
    ctx->current_band = &bk4819_bands[0];
    ctx->frequency = 14550000; // 145.5 МГц (диапазон FM)
    ctx->gain = AUTO_GAIN_INDEX;
    break;
  case RADIO_SI4732:
    ctx->current_band = &si4732_bands[0];
    ctx->frequency = 710000; // 7.1 МГц (диапазон AM)
    break;
  default:
    break;
  }
}

// Проверка параметра для текущего диапазона
bool RADIO_IsParamValid(VFOContext *ctx, ParamType param, uint32_t value) {
  const FreqBand *band = ctx->current_band;
  if (!band)
    return false;

  switch (param) {
  case PARAM_FREQUENCY:
  case PARAM_TX_FREQUENCY:
    return (value >= band->min_freq && value <= band->max_freq);

  case PARAM_MODULATION:
    for (uint8_t i = 0; i < band->num_available_mods; i++) {
      if (band->available_mods[i] == value)
        return true;
    }
    return false;

  case PARAM_BANDWIDTH:
    for (uint8_t i = 0; i < band->num_available_bandwidths; i++) {
      if (band->available_bandwidths[i] == value)
        return true;
    }
    return false;

  case PARAM_GAIN:
    if (ctx->radio_type == RADIO_BK4819) {
      return value < ARRAY_SIZE(GAIN_TABLE); // Оставляем как есть (индекс)
    }
    if (ctx->radio_type == RADIO_SI4732) {
      return value <= 27; // 0..26 + auto
    }
    return false;

  case PARAM_POWER:
    return value <= TX_POW_HIGH;

  case PARAM_UPCONVERTER:
    return true; // Any frequency is valid

  default:
    return true;
  }
}

// Установка параметра (всегда uint32_t!)
void RADIO_SetParam(VFOContext *ctx, ParamType param, uint32_t value,
                    bool save_to_eeprom) {
  if (!RADIO_IsParamValid(ctx, param, value)) {
    LogC(LOG_C_RED, "[RADIO] ERR: %-12s -> %u", PARAM_NAMES(param), value);
    return;
  }

  uint32_t old_value = RADIO_GetParam(ctx, param);

  switch (param) {

  case PARAM_MODULATION:
    ctx->modulation = (ModulationType)value;
    if (ctx->current_band) {
      ctx->modulation_index = 0;
      for (uint8_t i = 0; i < ctx->current_band->num_available_mods; i++) {
        if (ctx->current_band->available_mods[i] == value) {
          ctx->modulation_index = i;
          break;
        }
      }
    }
    ctx->dirty[PARAM_MODULATION] = true;

    switch (ctx->modulation) {
    case MOD_LSB:
    case MOD_USB:
      RADIO_SetParam(ctx, PARAM_AFC, 0, save_to_eeprom);
      break;
    default:
      // RADIO_SetParam(ctx, PARAM_AFC, 8, save_to_eeprom);
      break;
    }

    switch (ctx->modulation) {
    case MOD_LSB:
    case MOD_USB:
    case MOD_AM:
      if (RADIO_GetParam(ctx, PARAM_FREQUENCY) < 30 * MHZ) {
        RADIO_SetParam(ctx, PARAM_BANDWIDTH, BK4819_FILTER_BW_6k,
                       save_to_eeprom);
      }
      break;
    default:
      RADIO_SetParam(ctx, PARAM_BANDWIDTH, BK4819_FILTER_BW_12k,
                     save_to_eeprom);
      break;
    }

    break;

  case PARAM_BANDWIDTH:
    ctx->bandwidth = (uint16_t)value;
    if (ctx->current_band) {
      ctx->bandwidth_index = 0;
      for (uint8_t i = 0; i < ctx->current_band->num_available_bandwidths;
           i++) {
        if (ctx->current_band->available_bandwidths[i] == value) {
          ctx->bandwidth_index = i;
          break;
        }
      }
    }
    ctx->dirty[PARAM_BANDWIDTH] = true;
    break;
  case PARAM_RX_CODE:
    ctx->code.value = value;
    break;
  case PARAM_TX_CODE:
    ctx->tx_state.code.value = value;
    break;

  case PARAM_PRECISE_F_CHANGE:
    ctx->preciseFChange = value;
    break;
  case PARAM_STEP:
    ctx->step = (Step)value;
    break;
  case PARAM_TX_FREQUENCY:
  case PARAM_TX_OFFSET:
    ctx->tx_state.frequency = value;
    break;
  case PARAM_TX_OFFSET_DIR:
    ctx->tx_state.offsetDirection = value;
    break;
  case PARAM_POWER: {
    ctx->power = value;
    ctx->tx_state.power_level = BANDS_CalculateOutputPower(
        ctx->power, RADIO_GetParam(ctx, PARAM_TX_FREQUENCY_FACT));

    ctx->tx_state.pa_enabled = true;
    ctx->dirty[PARAM_TX_POWER] = true;
    ctx->dirty[PARAM_TX_POWER_AMPLIFIER] = true;
  } break;
  case PARAM_TX_POWER:
    ctx->tx_state.power_level = value;
    break;
  case PARAM_TX_POWER_AMPLIFIER:
    ctx->tx_state.pa_enabled = value;
    break;

  case PARAM_FREQUENCY:
    ctx->frequency = value + ctx->upconverter;
    break;
  case PARAM_VOLUME:
    ctx->volume = (uint8_t)value;
    break;
  case PARAM_GAIN:
    ctx->gain = value;
    break;
  case PARAM_AFC:
    ctx->afc = value;
    break;
  case PARAM_AFC_SPD:
    ctx->afc_speed = value;
    break;
  case PARAM_AF_RX_300:
    gSettings.af_rx_300 = value;
    RADIO_ApplyAFResponse(false, false);
    SETTINGS_MarkDirty(SETTING_AF_RX_300);
    break;
  case PARAM_AF_RX_3K:
    gSettings.af_rx_3k = value;
    RADIO_ApplyAFResponse(false, true);
    SETTINGS_MarkDirty(SETTING_AF_RX_3K);
    break;
  case PARAM_AF_TX_300:
    gSettings.af_tx_300 = value;
    RADIO_ApplyAFResponse(true, false);
    SETTINGS_MarkDirty(SETTING_AF_TX_300);
    break;
  case PARAM_AF_TX_3K:
    gSettings.af_tx_3k = value;
    RADIO_ApplyAFResponse(true, true);
    SETTINGS_MarkDirty(SETTING_AF_TX_3K);
    break;
  case PARAM_DEV:
    ctx->dev = value;
    break;
  case PARAM_MIC:
    ctx->mic = value;
    break;
  case PARAM_AGC:
    ctx->agc = value;
    break;
  case PARAM_XTAL:
    ctx->xtal = (XtalMode)value;
    break;
  case PARAM_SCRAMBLER:
    ctx->scrambler = value;
    break;
  case PARAM_FILTER:
    ctx->filter = (Filter)value;
    break;
  case PARAM_UPCONVERTER:
    ctx->upconverter = value;
    // Need to retune with new upconverter value
    ctx->dirty[PARAM_FREQUENCY] = true;
    break;
  case PARAM_SQUELCH_VALUE:
    ctx->squelch.value = value;
    break;
  case PARAM_SQUELCH_TYPE:
    ctx->squelch.type = value;
    break;
  case PARAM_RADIO:
    ctx->radio_type = value;
    // RADIO_UpdateCurrentBand(ctx);
    for (uint8_t i = 0; i < PARAM_COUNT; ++i) {
      ctx->dirty[i] = true;
    }
    break;
  case PARAM_FREQUENCY_FACT:
  case PARAM_TX_FREQUENCY_FACT:
  case PARAM_TX_STATE:
  case PARAM_RSSI:
  case PARAM_NOISE:
  case PARAM_GLITCH:
  case PARAM_SNR:
  case PARAM_COUNT:
    return;
  }

  if (param == PARAM_FREQUENCY || param == PARAM_BANDWIDTH ||
      param == PARAM_MODULATION) {
    RADIO_UpdateCurrentBand(ctx);
    RADIO_ApplyCorrections(ctx, save_to_eeprom);
  }

#ifdef DEBUG_PARAMS
  LogC(LOG_C_WHITE, "[SET] %-12s -> %s%s", PARAM_NAMES(param),
       RADIO_GetParamValueString(ctx, param), save_to_eeprom ? " [W]" : "");
#endif /* ifdef DEBUG_PARAMS */

  // Always mark dirty to ensure param is applied to hardware,
  // even if value hasn't changed (important for initial setup)
  ctx->dirty[param] = true;

  // Если значение изменилось и требуется сохранение - устанавливаем флаг
  if (save_to_eeprom && (old_value != value)) {
    LogC(LOG_C_BRIGHT_YELLOW, "[RADIO] SAVE %s", PARAM_NAMES(param));
    ctx->save_to_eeprom = true;
    ctx->last_save_time = Now();
  }
}

uint32_t RADIO_GetParam(const VFOContext *ctx, ParamType param) {
  switch (param) {
  case PARAM_RX_CODE:
    return ctx->code.value;
  case PARAM_TX_CODE:
    return ctx->tx_state.code.value;
  case PARAM_TX_OFFSET:
    return ctx->tx_state.frequency;
  case PARAM_TX_OFFSET_DIR:
    return ctx->tx_state.offsetDirection;
  case PARAM_TX_FREQUENCY:
    return ctx->tx_state.frequency;
  case PARAM_TX_FREQUENCY_FACT:
    return getRealTxFreq(ctx);
  case PARAM_TX_POWER_AMPLIFIER:
    return ctx->tx_state.pa_enabled;
  case PARAM_TX_POWER:
    return ctx->tx_state.power_level;
  case PARAM_TX_STATE:
    return ctx->tx_state.is_active;
  case PARAM_RSSI:
    return vfo->msm.rssi;
  case PARAM_NOISE:
    return vfo->msm.noise;
  case PARAM_GLITCH:
    return vfo->msm.glitch;
  case PARAM_SNR:
    return vfo->msm.snr;
  case PARAM_FREQUENCY:
    return ctx->frequency - ctx->upconverter;
  case PARAM_FREQUENCY_FACT:
    return ctx->frequency;
  case PARAM_PRECISE_F_CHANGE:
    return ctx->preciseFChange;
  case PARAM_MODULATION:
    return ctx->modulation;
  case PARAM_BANDWIDTH:
    return ctx->bandwidth;
  case PARAM_STEP:
    return ctx->step;
  case PARAM_VOLUME:
    return ctx->volume;
  case PARAM_GAIN:
    return ctx->gain;
  case PARAM_SQUELCH_TYPE:
    return ctx->squelch.type;
  case PARAM_SQUELCH_VALUE:
    return ctx->squelch.value;
  case PARAM_RADIO:
    return ctx->radio_type;
  case PARAM_POWER:
    return ctx->power;
  case PARAM_AFC:
    return ctx->afc;
  case PARAM_AFC_SPD:
    return ctx->afc_speed;
  case PARAM_AF_RX_300:
    return gSettings.af_rx_300;
  case PARAM_AF_RX_3K:
    return gSettings.af_rx_3k;
  case PARAM_AF_TX_300:
    return gSettings.af_tx_300;
  case PARAM_AF_TX_3K:
    return gSettings.af_tx_3k;
  case PARAM_XTAL:
    return ctx->xtal;
  case PARAM_SCRAMBLER:
    return ctx->scrambler;
  case PARAM_FILTER:
    return ctx->filter;
  case PARAM_MIC:
    return ctx->mic;
  case PARAM_AGC:
    return ctx->agc;
  case PARAM_DEV:
    return ctx->dev;
  case PARAM_UPCONVERTER:
    return ctx->upconverter;
  case PARAM_COUNT:
    break;
  }
  return 0;
}

bool RADIO_AdjustParam(VFOContext *ctx, ParamType param, uint32_t inc,
                       bool save_to_eeprom) {
  const FreqBand *band = ctx->current_band;
  if (!band) {
    return false;
  }

  uint32_t v = RADIO_GetParam(ctx, param);

  // Индексные параметры (зависят от band, не от простого диапазона)
  if (param == PARAM_MODULATION) {
    if (band->num_available_mods == 0)
      return false;
    ctx->modulation_index =
        AdjustU(ctx->modulation_index, 0, band->num_available_mods, inc);
    RADIO_SetParam(ctx, param, band->available_mods[ctx->modulation_index],
                   save_to_eeprom);
    RADIO_ApplySettings(ctx);
    return true;
  }

  if (param == PARAM_BANDWIDTH) {
    if (band->num_available_bandwidths == 0)
      return false;
    ctx->bandwidth_index =
        AdjustU(ctx->bandwidth_index, 0, band->num_available_bandwidths, inc);
    RADIO_SetParam(ctx, param, band->available_bandwidths[ctx->bandwidth_index],
                   save_to_eeprom);
    RADIO_ApplySettings(ctx);
    return true;
  }

  if (param == PARAM_TX_OFFSET) {
    RADIO_SetParam(ctx, param, AdjustU(v, 0, BK4819_F_MAX, inc),
                   save_to_eeprom);
    RADIO_ApplySettings(ctx);
    return true;
  }

  // Параметры с диапазоном из FreqBand
  if (param == PARAM_FREQUENCY || param == PARAM_TX_FREQUENCY) {
    RADIO_SetParam(ctx, param, AdjustU(v, band->min_freq, band->max_freq, inc),
                   save_to_eeprom);
    RADIO_ApplySettings(ctx);
    return true;
  }

  // Параметры с динамическим диапазоном (зависит от радио)
  if (param == PARAM_GAIN) {
    uint32_t ma;
    if (ctx->radio_type == RADIO_BK4819) {
      ma = ARRAY_SIZE(GAIN_TABLE);
    } else if (ctx->radio_type == RADIO_SI4732) {
      ma = 28;
    } else {
      return false;
    }
    RADIO_SetParam(ctx, param, AdjustU(v, 0, ma, inc), save_to_eeprom);
    RADIO_ApplySettings(ctx);
    return true;
  }

  // Остальные параметры — берём диапазон из таблицы PARAM_DESC
  const ParamDesc *d = &PARAM_DESC[param];
  if (d->max_val == 0) {
    // read-only или неподдерживаемый
    LogC(LOG_C_RED, "[RADIO] ERR: range %s", PARAM_NAMES(param));
    return false;
  }

  RADIO_SetParam(ctx, param, AdjustU(v, d->min_val, d->max_val, inc),
                 save_to_eeprom);
  RADIO_ApplySettings(ctx);
  return true;
}

bool RADIO_IncDecParam(VFOContext *ctx, ParamType param, bool inc,
                       bool save_to_eeprom) {
  uint32_t v = 1;
  if (param == PARAM_FREQUENCY) {
    v = StepFrequencyTable[ctx->step];
  }
  if (param == PARAM_RADIO) {
    v = RADIO_GetParam(ctx, PARAM_RADIO);
    Radio old_radio = v; // Сохраняем старый тип приёмника
    v = v == RADIO_BK4819 ? (RADIO_HasSi() ? RADIO_SI4732 : RADIO_BK1080)
                          : RADIO_BK4819;

    // Если тип приёмника действительно меняется
    if (old_radio != v) {
      // 1. Выключаем аудио с текущего приёмника
      vfo->is_open = false;
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);

      // 2. Устанавливаем новый тип приёмника
      RADIO_SetParam(ctx, param, v, save_to_eeprom);

      // 3. Применяем настройки (переключит hardware)
      RADIO_ApplySettings(ctx);

      // 4. Проверяем и обновляем шумодав на новом приёмнике
      RADIO_UpdateSquelch(gRadioState);
    }
    return true;
  }
  if (param == PARAM_DEV) {
    v = 10;
  }
  return RADIO_AdjustParam(ctx, param, inc ? v : -v, save_to_eeprom);
}

typedef bool (*spf_ptr)(VFOContext *, ParamType);

static spf_ptr setParamForRadio[] = {
    [RADIO_BK4819] = &setParamBK4819,
    [RADIO_BK1080] = &setParamBK1080,
    [RADIO_SI4732] = &setParamSI4732,
};

void RADIO_ApplyAFResponse(bool tx, bool is3k) {
  int8_t v = tx ? (is3k ? gSettings.af_tx_3k : gSettings.af_tx_300)
                : (is3k ? gSettings.af_rx_3k : gSettings.af_rx_300);
  BK4819_SetAFResponse(tx, is3k, v - 4);
}

void RADIO_ApplyAllAFResponse(void) {
  RADIO_ApplyAFResponse(false, false);
  RADIO_ApplyAFResponse(false, true);
  RADIO_ApplyAFResponse(true, false);
  RADIO_ApplyAFResponse(true, true);
}

// Применение настроек
// Параметры, которые не передаются в железо (только в ctx)
#define SKIP_MASK \
    ((1u << PARAM_STEP)             | (1u << PARAM_POWER)           | \
     (1u << PARAM_TX_FREQUENCY)     | (1u << PARAM_TX_OFFSET)       | \
     (1u << PARAM_TX_OFFSET_DIR)    | (1u << PARAM_TX_STATE)        | \
     (1u << PARAM_TX_CODE)          | (1u << PARAM_RX_CODE)         | \
     (1u << PARAM_RSSI)             | (1u << PARAM_NOISE)           | \
     (1u << PARAM_GLITCH)           | (1u << PARAM_SNR)             | \
     (1u << PARAM_PRECISE_F_CHANGE))

void RADIO_ApplySettings(VFOContext *ctx) {
  if (ctx->dirty[PARAM_RADIO]) {
    LogC(LOG_C_BRIGHT_MAGENTA, "[RADIO] =%s",
         RADIO_GetParamValueString(ctx, PARAM_RADIO));
    ctx->dirty[PARAM_RADIO] = false;

    ExtendedVFOContext *ev = RADIO_GetCurrentVFO(gRadioState);
    RXSW_SwitchTo(&gRadioState->rx_switch, ctx, ev ? ev->is_open : false);
  }

  const bool needSetupToneDetection =
      (ctx->dirty[PARAM_RX_CODE] || ctx->dirty[PARAM_TX_CODE] ||
       ctx->dirty[PARAM_TX_STATE] || ctx->dirty[PARAM_RADIO]) &&
      ctx->radio_type == RADIO_BK4819;

  for (uint8_t p = 0; p < PARAM_COUNT; ++p) {
    if (!ctx->dirty[p])
      continue;

    ctx->dirty[p] = false;  // сбрасываем сразу — и для skip, и для handled

    if (SKIP_MASK & (1u << p))
      continue;

    if (!setParamForRadio[ctx->radio_type](ctx, p)) {
#ifdef DEBUG_PARAMS
      /* LogC(LOG_C_YELLOW, "[W] Param %s not set for %s", PARAM_NAMES(p),
           RADIO_NAMES[ctx->radio_type]); */
#endif
      continue;
    }
#ifdef DEBUG_PARAMS
    LogC(LOG_C_BRIGHT_WHITE, "[SET] %-12s -> %s", PARAM_NAMES(p),
         RADIO_GetParamValueString(ctx, p));
#endif
  }

  if (needSetupToneDetection) {
    RADIO_SetupToneDetection(ctx);
  }
}

// Начать передачу
bool RADIO_StartTX(VFOContext *ctx) {
  TXStatus status = checkTX(ctx);
  if (status != TX_ON) {
    ctx->tx_state.last_error = status;
    return false;
  }
  ctx->tx_state.last_error = TX_UNKNOWN;
  if (ctx->tx_state.is_active) {
    return true;
  }

  uint8_t power = ctx->tx_state.power_level;

  // Properly close VFO through RXSW before TX
  vfo->is_open = false;
  RXSW_SwitchTo(&gRadioState->rx_switch, ctx, false);

  // Save and exit FSK mode if active (FSK conflicts with TX)
  bool fsk_was_active = (gCurrentApp != APP_MESSENGER);
  if (fsk_was_active) {
    RF_ExitFsk();
  }

  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

  uint32_t txF = getRealTxFreq(ctx);
  BK4819_SelectFilter(txF);
  BK4819_TuneTo(txF, true);

  BOARD_ToggleRed(gSettings.brightness > 1);
  // GPIO_TurnOffBacklight();
  BK4819_PrepareTransmit();

  SYSTICK_DelayMs(10);
  BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, ctx->tx_state.pa_enabled);
  SYSTICK_DelayMs(5);
  BK4819_SetupPowerAmplifier(power, txF);
  SYSTICK_DelayMs(10);

  enableCxCSS(ctx);
  ctx->tx_state.is_active = true;
  return true;
}

// Завершить передачу
void RADIO_StopTX(VFOContext *ctx) {
  ctx->tx_state.last_error = TX_UNKNOWN;
  if (!ctx->tx_state.is_active) {
    return;
  }

  BK4819_ExitDTMF_TX(true); // also prepares to tx ste

  sendEOT();

  // сперва отрубаем несучку
  BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
  BK4819_SetupPowerAmplifier(0, 0);

  // затем отрубаем тон, так в эфире последнее будет тон,
  // а не несучка после отключения тона
  BK4819_TurnsOffTones_TurnsOnRX();

  ctx->tx_state.is_active = false;
  BOARD_ToggleRed(false);
  // GPIO_TurnOnBacklight();

  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

  RADIO_SetupToneDetection(ctx);
  BK4819_SelectFilter(ctx->frequency);
  BK4819_TuneTo(ctx->frequency, true);

  // Restore FSK mode if it was active before TX
  if (gCurrentApp != APP_MESSENGER) {
    RF_EnterFsk();
  }
}

void RADIO_ToggleTX(VFOContext *ctx, bool on) {
  if (on) {
    RADIO_StartTX(ctx);
  } else {
    RADIO_StopTX(ctx);
  }
}

bool RADIO_IsSSB(const VFOContext *ctx) {
  return ctx->modulation == MOD_LSB || ctx->modulation == MOD_USB;
}

// Initialize radio state
void RADIO_InitState(RadioState *state, uint8_t num_vfos) {
  Log("[RADIO] InitState");
  memset(state, 0, sizeof(RadioState));
  state->num_vfos = (num_vfos > MAX_VFOS) ? MAX_VFOS : num_vfos;

  state->primary_vfo_index = state->active_vfo_index = gSettings.activeVFO;

  for (uint8_t i = 0; i < state->num_vfos; i++) {
    memset(&state->vfos[i].context, 0, sizeof(VFOContext));
    state->vfos[i].mode = MODE_VFO;
    state->vfos[i].channel_index = 0;
    state->vfos[i].is_active = (i == state->primary_vfo_index);
    state->vfos[i].is_open = false;
  }

  updateContext();

  state->last_scan_time = 0;
  state->multiwatch_enabled = gSettings.mWatch;
}

// Функция проверки необходимости сохранения и собственно сохранения
void RADIO_CheckAndSaveVFO(RadioState *state) {
  for (uint8_t i = 0; i < state->num_vfos; ++i) {
    VFOContext *ctx = &state->vfos[i].context;

    if (ctx->save_to_eeprom &&
        (Now() - ctx->last_save_time >= RADIO_SAVE_DELAY_MS)) {
      LogC(LOG_C_BRIGHT_YELLOW, "TRYING TO SAVE PARAM");

      VFO vfo;
      RADIO_SaveVFOToStorage(state, i, &vfo);

      saveVfo(i, &vfo);

      ctx->save_to_eeprom = false;
    }
  }
}

static bool RADIO_SwitchVFOTemp(RadioState *state, uint8_t vfo_index) {
  if (vfo_index >= state->num_vfos) {
    return false;
  }

  // Log("RADIO_SwitchVFOTemp %u", vfo_index);

  // Deactivate current VFO
  // state->vfos[state->active_vfo_index].is_active = false;

  VFOContext *oldCtx = &state->vfos[state->active_vfo_index].context;
  VFOContext *newCtx = &state->vfos[vfo_index].context;

  for (uint8_t p = 0; p < PARAM_COUNT; ++p) {
    newCtx->dirty[p] = RADIO_GetParam(oldCtx, p) != RADIO_GetParam(newCtx, p);
  }

  // Activate new VFO
  state->vfos[vfo_index].is_active = true;
  state->active_vfo_index = vfo_index;
  gRedrawScreen = true;

  // Apply settings for the new VFO
  RADIO_ApplySettings(&state->vfos[vfo_index].context);

  return true;
}

// Switch to a different VFO
bool RADIO_SwitchVFO(RadioState *state, uint8_t vfo_index) {
  if (vfo_index >= state->num_vfos) {
    return false;
  }

  Log("[RADIO] SwitchVFO");

  RADIO_CheckAndSaveVFO(state);

  // Deactivate current VFO
  state->vfos[state->active_vfo_index].is_active = false;

  VFOContext *oldCtx = &state->vfos[state->active_vfo_index].context;
  VFOContext *newCtx = &state->vfos[vfo_index].context;

  for (uint8_t p = 0; p < PARAM_COUNT; ++p) {
    newCtx->dirty[p] = RADIO_GetParam(oldCtx, p) != RADIO_GetParam(newCtx, p);
  }

  // mute previous vfo (fast fix)
  state->vfos[state->active_vfo_index].is_open = false;
  RADIO_SwitchAudioToVFO(state, state->active_vfo_index);
  RADIO_SwitchAudioToVFO(state, vfo_index);

  // Activate new VFO
  state->vfos[vfo_index].is_active = true;
  state->primary_vfo_index = state->active_vfo_index = vfo_index;
  updateContext();

  // Apply settings for the new VFO
  RADIO_ApplySettings(&state->vfos[vfo_index].context);

  gSettings.activeVFO = vfo_index;
  SETTINGS_DelayedSave();

  return true;
}

// Общие умолчания, одинаковые для VFO и Channel
static void applyGlobalDefaults(VFOContext *ctx) {
  // mic, deviation, upconverter are now per-VFO - don't override from gSettings
  // Only apply if not loading from storage (i.e., fresh init)
  RADIO_SetParam(ctx, PARAM_XTAL, XTAL_2_26M, false);
  RADIO_SetParam(ctx, PARAM_SCRAMBLER, 0, false);
  RADIO_SetParam(ctx, PARAM_AFC, 0, false);
  RADIO_SetParam(ctx, PARAM_AFC_SPD, 63, false);
  RADIO_SetParam(ctx, PARAM_FILTER, FILTER_AUTO, false);
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
}

static void setCommonParamsFromVFO(VFOContext *ctx, const VFO *storage) {
  // Инициализируем radio/frequency/band до SetParam, чтобы IsParamValid работал
  ctx->radio_type = storage->radio;
  ctx->modulation = storage->modulation;
  ctx->frequency = storage->rxF;
  RADIO_UpdateCurrentBand(ctx);
  RADIO_ApplyCorrections(ctx, false);

  RADIO_SetParam(ctx, PARAM_RADIO, storage->radio, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, storage->rxF, false);
  RADIO_SetParam(ctx, PARAM_BANDWIDTH, storage->bw, false);
  RADIO_SetParam(ctx, PARAM_STEP, storage->step, false);
  RADIO_SetParam(ctx, PARAM_GAIN, storage->gainIndex, false);
  RADIO_SetParam(ctx, PARAM_MODULATION, storage->modulation, false);
  RADIO_SetParam(ctx, PARAM_POWER, storage->power, false);
  RADIO_SetParam(ctx, PARAM_SQUELCH_TYPE, storage->squelch.type, false);
  RADIO_SetParam(ctx, PARAM_SQUELCH_VALUE, storage->squelch.value, false);
  RADIO_SetParam(ctx, PARAM_TX_FREQUENCY, storage->txF, false);
  RADIO_SetParam(ctx, PARAM_TX_OFFSET, storage->txF, false);
  RADIO_SetParam(ctx, PARAM_TX_OFFSET_DIR, storage->offsetDir, false);
  RADIO_SetParam(ctx, PARAM_MIC, storage->mic, false);
  RADIO_SetParam(ctx, PARAM_DEV, storage->deviation * 10, false);
  RADIO_SetParam(ctx, PARAM_UPCONVERTER, storage->upconverter, false);

  strcpy(ctx->name, storage->name);
  ctx->code = storage->code.rx;
  ctx->tx_state.code = storage->code.tx;
  ctx->tx_state.is_active = false;
  ctx->tx_state.last_error = TX_UNKNOWN;

  applyGlobalDefaults(ctx);
}

static void setCommonParamsFromCh(VFOContext *ctx, const CH *storage) {
  ctx->radio_type = storage->radio;
  ctx->modulation = storage->modulation;
  ctx->frequency = storage->rxF;
  RADIO_UpdateCurrentBand(ctx);
  RADIO_ApplyCorrections(ctx, false);

  RADIO_SetParam(ctx, PARAM_RADIO, storage->radio, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, storage->rxF, false);
  RADIO_SetParam(ctx, PARAM_BANDWIDTH, storage->bw, false);
  RADIO_SetParam(ctx, PARAM_GAIN, storage->gainIndex, false);
  RADIO_SetParam(ctx, PARAM_MODULATION, storage->modulation, false);
  RADIO_SetParam(ctx, PARAM_POWER, storage->power, false);
  RADIO_SetParam(ctx, PARAM_SQUELCH_TYPE, storage->squelch.type, false);
  RADIO_SetParam(ctx, PARAM_SQUELCH_VALUE, storage->squelch.value, false);
  RADIO_SetParam(ctx, PARAM_TX_FREQUENCY, storage->txF, false);
  RADIO_SetParam(ctx, PARAM_TX_OFFSET, storage->txF, false);
  RADIO_SetParam(ctx, PARAM_TX_OFFSET_DIR, storage->offsetDir, false);

  strcpy(ctx->name, storage->name);
  ctx->code = storage->code.rx;
  ctx->tx_state.code = storage->code.tx;
  ctx->tx_state.is_active = false;
  ctx->tx_state.last_error = TX_UNKNOWN;

  // For channels, apply global defaults (channels don't store mic/dev/upconv)
  RADIO_SetParam(ctx, PARAM_MIC, gSettings.mic, false);
  RADIO_SetParam(ctx, PARAM_DEV, gSettings.deviation * 10, false);
  RADIO_SetParam(ctx, PARAM_UPCONVERTER, gSettings.upconverter, false);
  applyGlobalDefaults(ctx);
}

// Load VFO settings from EEPROM storage
void RADIO_LoadVFOFromStorage(RadioState *state, uint8_t vfo_index,
                              const VFO *storage) {
  if (vfo_index >= state->num_vfos)
    return;
  LogC(LOG_C_BG_BLUE, "[RADIO] MEM -> VFO %u", vfo_index + 1);

  ExtendedVFOContext *vfo = &state->vfos[vfo_index];
  vfo->mode = storage->isChMode;
  VFOContext *ctx = &vfo->context;

  // Set basic parameters
  setCommonParamsFromVFO(ctx, storage);

  if (vfo->mode == MODE_CHANNEL) {
    vfo->channel_index = storage->channel;
  }
}

// Save VFO settings to EEPROM storage
void RADIO_SaveVFOToStorage(const RadioState *state, uint8_t vfo_index,
                            VFO *storage) {
  if (vfo_index >= state->num_vfos)
    return;
  LogC(LOG_C_BRIGHT_CYAN, "[RADIO] VFO -> MEM");

  const ExtendedVFOContext *vfo = &state->vfos[vfo_index];
  const VFOContext *ctx = &vfo->context;

  storage->isChMode = vfo->mode;
  storage->channel = vfo->channel_index;

  storage->radio = ctx->radio_type;

  storage->rxF = ctx->frequency;
  storage->step = ctx->step;

  storage->power = ctx->power;
  storage->bw = ctx->bandwidth;
  storage->modulation = ctx->modulation;
  storage->gainIndex = ctx->gain;
  storage->squelch = ctx->squelch;

  storage->code.rx = ctx->code;
  storage->code.tx = ctx->tx_state.code;

  storage->txF = ctx->tx_state.frequency;
  storage->offsetDir = ctx->tx_state.offsetDirection;

  storage->mic = ctx->mic;
  storage->deviation = ctx->dev / 10; // Convert 0-2550 to 0-255
  storage->upconverter = ctx->upconverter;
}

bool RADIO_SaveCurrentVFO(RadioState *state) {
  if (!state)
    return false;

  uint8_t current = state->active_vfo_index;
  VFO storage;

  RADIO_SaveVFOToStorage(state, current, &storage);
  saveVfo(current, &storage);

  return true;
}

// Save all VFOs to storage (used during app switching)
bool RADIO_SaveAllVFOs(RadioState *state) {
  if (!state)
    return false;

  VFO storage;
  for (uint8_t i = 0; i < state->num_vfos; i++) {
    RADIO_SaveVFOToStorage(state, i, &storage);
    saveVfo(i, &storage);
  }

  return true;
}

// Load channel into VFO
void RADIO_LoadChannelToVFO(RadioState *state, uint8_t vfo_index,
                            uint16_t channel_index) {
  if (vfo_index >= state->num_vfos) {
    return;
  }

  LogC(LOG_C_BRIGHT_CYAN, "[RADIO] CH %u -> VFO", channel_index);

  ExtendedVFOContext *vfo = &state->vfos[vfo_index];
  VFOContext *ctx = &vfo->context;
  CH channel;
  STORAGE_LOAD("Channels.ch", channel_index, &channel);

  vfo->mode = MODE_CHANNEL;
  vfo->channel_index = channel_index;

  setCommonParamsFromCh(ctx, &channel);
}

bool RADIO_NextChannel(bool next) {
  if (vfo->mode != MODE_CHANNEL) {
    RADIO_ToggleVFOMode(gRadioState, gRadioState->active_vfo_index);
  }
  uint16_t initialChIndex = vfo->channel_index;

  while (1) {
    vfo->channel_index = (vfo->channel_index + 1) % 4096;
    if (vfo->channel_index == initialChIndex) {
      return false;
    }
    CH ch;
    STORAGE_LOAD("Channels.ch", vfo->channel_index, &ch);
    if (IsReadable(ch.name)) {
      setCommonParamsFromCh(ctx, &ch);
      RADIO_ApplySettings(&vfo->context);

      // Помечаем для сохранения в EEPROM
      ctx->save_to_eeprom = true;
      ctx->last_save_time = Now();

      gRedrawScreen = true;

      return true;
    }
  }
  return true;
}

/**
 * @brief Переключает режим работы VFO между частотным и канальным
 * @param state Указатель на состояние радио
 * @param vfo_index Индекс VFO (0..MAX_VFOS-1)
 * @return true если переключение успешно, false при ошибке
 */
bool RADIO_ToggleVFOMode(RadioState *state, uint8_t vfo_index) {
  // Проверка допустимости индекса VFO
  if (vfo_index >= state->num_vfos) {
    return false;
  }

  ExtendedVFOContext *vfo = &state->vfos[vfo_index];
  VFOContext *ctx = &vfo->context;

  // Определяем новый режим (инвертируем текущий)
  VFOMode new_mode = vfo->mode == MODE_CHANNEL ? MODE_VFO : MODE_CHANNEL;
  VFO ch;
  if (new_mode == MODE_CHANNEL) {
    STORAGE_LOAD("Channels.ch", vfo->vfo_ch_index, &ch);
  } else {
    loadVfo(vfo_index, &ch);
  }

  ch.isChMode = new_mode == MODE_CHANNEL;
  LogC(LOG_C_BRIGHT_CYAN, "[RADIO] VFOMode = %s", ch.isChMode ? "MR" : "VFO");

  saveVfo(vfo_index, &ch);

  if (new_mode == MODE_CHANNEL) {
    // TODO: if wrong CH num in VFO, load one from current SL
    RADIO_LoadChannelToVFO(state, vfo_index, ch.channel);
  } else {
    RADIO_LoadVFOFromStorage(state, vfo_index, &ch);
  }
  RADIO_ApplySettings(&vfo->context);

  // Помечаем для сохранения в EEPROM
  ctx->save_to_eeprom = true;
  ctx->last_save_time = Now();

  gRedrawScreen = true;

  return true;
}

// Toggle multiwatch on/off
void RADIO_ToggleMultiwatch(RadioState *state, bool enable) {
  if (state->multiwatch_enabled == enable) {
    return;
  }
  state->multiwatch_enabled = enable;
  if (!enable) {
    // Return to the primary VFO when disabling multiwatch
    RADIO_SwitchVFO(state, 0);
  }
}

// Check if a VFO is a broadcast receiver
static bool isBroadcastReceiver(const VFOContext *ctx) {
  return (ctx->radio_type == RADIO_SI4732 || ctx->radio_type == RADIO_BK1080);
}

bool RADIO_CheckSquelch(VFOContext *ctx) {
  if (ctx->tx_state.is_active)
    return false;
  if (gMonitorMode)
    return true;
  if (ctx->radio_type == RADIO_BK4819) {
    return BK4819_IsSquelchOpen();
  }
  return gShowAllRSSI ? RADIO_GetSNR(ctx) > ctx->squelch.value : true;
}

static void RADIO_UpdateMeasurement(ExtendedVFOContext *vfo) {
  // Log("Update MSM");
  VFOContext *ctx = &vfo->context;
  vfo->msm.f = ctx->frequency;
  vfo->msm.rssi = RADIO_GetRSSI(ctx);
  vfo->msm.noise = BK4819_GetNoise();
  vfo->msm.glitch = BK4819_GetGlitch();
  vfo->msm.snr = RADIO_GetSNR(ctx);
  vfo->msm.open = RADIO_CheckSquelch(ctx);
  if (!gMonitorMode && ctx->radio_type == RADIO_BK4819) {
    LOOT_Update(&vfo->msm);
  }
}

void RADIO_UpdateSquelch(RadioState *state) {
  RADIO_UpdateMeasurement(&state->vfos[state->active_vfo_index]);
  if (vfo->is_open != vfo->msm.open) {
    gRedrawScreen = true; // TODO: mv
    vfo->is_open = vfo->msm.open;
    RADIO_SwitchAudioToVFO(state, state->active_vfo_index);
  }
}
// Update multiwatch state (should be called periodically)

void RADIO_UpdateMultiwatch(RadioState *state) {
  if (!state->multiwatch_enabled || gSettings.mWatch == 0) {
    state->scan_state = RADIO_SCAN_STATE_IDLE;
    return;
  }

  static int8_t current_scan_vfo = 0;
  static uint32_t last_scan_time = 0;
  uint32_t current_time = Now();

  switch (state->scan_state) {
  case RADIO_SCAN_STATE_IDLE:
    // Log("IDLE");
    // Начинаем новый цикл сканирования
    current_scan_vfo = -1;
    state->scan_state = RADIO_SCAN_STATE_SWITCHING;
    break;

  case RADIO_SCAN_STATE_SWITCHING:
    // RADIO_CheckAndSaveVFO(gRadioState);
    // Ищем следующий VFO для сканирования (пропускаем активный и вещательные)
    do {
      current_scan_vfo = (current_scan_vfo + 1) % state->num_vfos;
    } while (current_scan_vfo == state->active_vfo_index ||
             isBroadcastReceiver(&state->vfos[current_scan_vfo].context));

    // Временно переключаемся
    RADIO_SwitchVFOTemp(state, current_scan_vfo);
    // Log("SW %u", state->vfos[current_scan_vfo].context.frequency);
    state->scan_state = RADIO_SCAN_STATE_WARMUP;
    last_scan_time = current_time;
    break;

  case RADIO_SCAN_STATE_WARMUP:
    // Log("WU");
    // Ждем стабилизации сигнала
    if (last_scan_time && current_time - last_scan_time >= SQL_DELAY) {
      state->scan_state = RADIO_SCAN_STATE_MEASURING;
    }
    break;

  case RADIO_SCAN_STATE_MEASURING:
    // Log("ME");
    // Выполняем замер
    RADIO_UpdateMeasurement(&state->vfos[current_scan_vfo]);
    // Log("MSM: %u", state->vfos[current_scan_vfo].msm.open);
    state->scan_state = RADIO_SCAN_STATE_DECISION;
    break;

  case RADIO_SCAN_STATE_DECISION: {
    // Log("DEC");
    // Только для режима с автопереключением
    ExtendedVFOContext *scanned = &state->vfos[current_scan_vfo];
    ExtendedVFOContext *active = &state->vfos[state->active_vfo_index];
    if (gSettings.mWatch == 2) {

      // Условия переключения:
      bool should_switch =
          scanned->msm.open &&
          (scanned->msm.rssi > active->msm.rssi + 3 || // Гистерезис 3dB
           !active->msm.open) &&
          scanned->msm.rssi > 0;

      if (should_switch) {
        RADIO_SwitchVFO(state, current_scan_vfo);
      }
    } else {
      if (scanned->msm.open != scanned->is_open) {
        scanned->is_open = scanned->msm.open;
        RADIO_SwitchAudioToVFO(state, current_scan_vfo);
      }
      if (scanned->msm.open) {
        // Log("OPEN!!!");
        state->scan_state = RADIO_SCAN_STATE_WARMUP;
        last_scan_time = Now();
        return;
      }
    }

    // Переходим к следующему VFO или завершаем цикл
    if (current_scan_vfo >= state->num_vfos - 1) {
      last_scan_time = 0; // NOTE: IMPORTANT
      state->scan_state = RADIO_SCAN_STATE_IDLE;
    } else {
      state->scan_state = RADIO_SCAN_STATE_SWITCHING;
    }
    break;
  }
  }
}

void RADIO_LoadVFOs(RadioState *state) {
  Log("[RADIO] LoadVFOs (multiple from storage)");

  initVfoFile();

  RXSW_Init(&state->rx_switch);

  VFO vfos[4];
  Storage_LoadMultiple(vfosFileName, 0, vfos, sizeof(VFO), 4);

  // Загружаем VFO
  uint8_t vfoIdx = 0;
  for (uint8_t i = 0; i < MAX_VFOS; ++i) {
    state->vfos[vfoIdx].vfo_ch_index = i;

    if (vfos[i].isChMode) {
      RADIO_LoadChannelToVFO(state, vfoIdx, vfos[i].channel);
    } else {
      RADIO_LoadVFOFromStorage(state, vfoIdx, &vfos[i]);
    }
    vfoIdx++;
  }
  state->num_vfos = vfoIdx;

  VFOContext *ctx = &state->vfos[state->active_vfo_index].context;
  for (uint8_t p = 0; p < PARAM_COUNT; ++p) {
    ctx->dirty[p] = true;
  }

  RADIO_ApplySettings(ctx);
  updateContext();
}

// Включаем/выключаем маршрутизацию аудио
void RADIO_EnableAudioRouting(RadioState *state, bool enable) {
  state->audio_routing_enabled = enable;
  if (!enable) {
    // При выключении возвращаем аудио на активный VFO
    RADIO_SwitchAudioToVFO(state, state->active_vfo_index);
  }
}

// Обновление маршрутизации аудио
void RADIO_UpdateAudioRouting(RadioState *state) {
  if (!state->audio_routing_enabled)
    return;

  // Если текущий VFO - вещательный приемник, оставляем аудио на нем
  if (isBroadcastReceiver(&state->vfos[state->active_vfo_index].context)) {
    return;
  }

  // Проверяем активность на других VFO
  for (uint8_t i = 0; i < state->num_vfos; i++) {
    if (i == state->active_vfo_index)
      continue;

    ExtendedVFOContext *vfo = &state->vfos[i];

    // Для вещательных приемников не переключаем аудио автоматически
    if (isBroadcastReceiver(&vfo->context))
      continue;

    bool has_activity = RADIO_CheckSquelch(&vfo->context);

    if (has_activity) {
      // Если нашли активность и это не текущий VFO - переключаем аудио
      if (state->last_active_vfo != i) {
        RADIO_SwitchAudioToVFO(state, i);
        state->last_active_vfo = i;
      }
      return;
    }
  }

  // Если активность не обнаружена - возвращаем аудио на основной VFO
  if (state->last_active_vfo != state->active_vfo_index) {
    RADIO_SwitchAudioToVFO(state, state->active_vfo_index);
    state->last_active_vfo = state->active_vfo_index;
  }
}

const char *RADIO_GetParamValueString(const VFOContext *ctx, ParamType param) {
  static char buf[16] = "unk";
  uint32_t v = RADIO_GetParam(ctx, param);
  switch (param) {
  case PARAM_RSSI:
    sprintf(buf, "%+ddB", Rssi2DBm(v));
    break;
  case PARAM_MODULATION:
    if (ctx->radio_type == RADIO_BK4819) {
      return MOD_NAMES_BK4819[ctx->modulation];
    }
    if (ctx->radio_type == RADIO_SI4732) {
      return MOD_NAMES_SI47XX[ctx->modulation];
    }
    return "WFM";
  case PARAM_TX_STATE:
    return TX_STATE_NAMES[ctx->tx_state.last_error];
  case PARAM_BANDWIDTH:
    if (ctx->radio_type == RADIO_BK4819) {
      return BW_NAMES_BK4819[v];
    }
    if (ctx->radio_type == RADIO_SI4732) {
      if (RADIO_IsSSB(ctx)) {
        return BW_NAMES_SI47XX_SSB[v];
      }
      return BW_NAMES_SI47XX[v];
    }
    return "?(WIP)";
  case PARAM_STEP:
sprintf(buf, "%d.%02d", StepFrequencyTable[v] / KHZ,
             StepFrequencyTable[v] % KHZ);
    break;
  case PARAM_FREQUENCY:
  case PARAM_FREQUENCY_FACT:
  case PARAM_TX_OFFSET:
  case PARAM_TX_FREQUENCY:
  case PARAM_TX_FREQUENCY_FACT:
    mhzToS(buf, v);
    break;
  case PARAM_RADIO:
    sprintf(buf, "%s", RADIO_NAMES[ctx->radio_type]);
    break;
  case PARAM_TX_OFFSET_DIR:
sprintf(buf, "%s",
             TX_OFFSET_NAMES[ctx->tx_state.offsetDirection]);
    break;
  case PARAM_GAIN:
    if (ctx->radio_type == RADIO_BK4819) {
      bkAttToS(buf, v);
      break;
    } else if (ctx->radio_type == RADIO_SI4732) {
      sprintf(buf, v == 0 ? "Auto" : "%u", v - 1);
      break;
    }
    sprintf(buf, "Auto");
    break;

  case PARAM_RX_CODE:
    PrintRTXCode(buf, ctx->code.type, ctx->code.value);
    break;

  case PARAM_TX_CODE:
    PrintRTXCode(buf, ctx->tx_state.code.type, ctx->tx_state.code.value);
    break;

  case PARAM_POWER:
    return TX_POWER_NAMES[ctx->power];
  case PARAM_FILTER:
    return FILTER_NAMES[ctx->filter];
  case PARAM_NOISE:
  case PARAM_GLITCH:
  case PARAM_SNR:
  case PARAM_TX_POWER:
  case PARAM_TX_POWER_AMPLIFIER:
  case PARAM_AFC:
  case PARAM_AFC_SPD:
  case PARAM_DEV:
  case PARAM_MIC:
  case PARAM_AGC:
  case PARAM_XTAL:
  case PARAM_SCRAMBLER:
  case PARAM_SQUELCH_VALUE:
  case PARAM_PRECISE_F_CHANGE:
  case PARAM_COUNT:
    sprintf(buf, "%u", v);
    break;
  case PARAM_AF_RX_300:
  case PARAM_AF_RX_3K:
  case PARAM_AF_TX_300:
  case PARAM_AF_TX_3K:
    sprintf(buf, "%+ddB", (int)v - 4);
    break;
  case PARAM_VOLUME:
    sprintf(buf, "%u%%", v);
    break;
  case PARAM_SQUELCH_TYPE:
    sprintf(buf, "%s", SQ_TYPE_NAMES[ctx->squelch.type]);
    break;
  case PARAM_UPCONVERTER:
    if (v == 0) {
      sprintf(buf, "Off");
    } else {
      sprintf(buf, "+%d.%02d", v / MHZ, (v % MHZ) / KHZ);
    }
    break;
  }
  return buf;
}

/**
 * @brief Получает номер текущего активного VFO
 * @param state Указатель на состояние радио
 * @return Номер VFO (0..MAX_VFOS-1) или 0xFF если не найдено
 */
uint8_t RADIO_GetCurrentVFONumber(const RadioState *state) {
  return state->primary_vfo_index;
}

/**
 * @brief Получает указатель на текущий активный VFO
 * @param state Указатель на состояние радио
 * @return Указатель на ExtendedVFOContext или NULL если ошибка
 */
ExtendedVFOContext *RADIO_GetCurrentVFO(RadioState *state) {
  uint8_t current = RADIO_GetCurrentVFONumber(state);
  return (current != 0xFF) ? &state->vfos[current] : NULL;
}

/**
 * @brief Получает константный указатель на текущий активный VFO
 * @param state Указатель на состояние радио
 * @return Константный указатель на ExtendedVFOContext или NULL если ошибка
 */
const ExtendedVFOContext *RADIO_GetCurrentVFOConst(const RadioState *state) {
  uint8_t current = RADIO_GetCurrentVFONumber(state);
  return (current != 0xFF) ? &state->vfos[current] : NULL;
}

void RADIO_FastSquelchUpdate() { vfo->is_open = RADIO_CheckSquelch(ctx); }

// Немедленно заглушить аудио (без изменения состояния squelch-логики)
void RADIO_MuteAudioNow(RadioState *state) {
  if (gMonitorMode) {
    return;
  }
  ExtendedVFOContext *ev = &state->vfos[state->active_vfo_index];
  if (!ev->is_open)
    return; // уже тихо
  ev->is_open = false;
  RXSW_SwitchTo(&state->rx_switch, &ev->context, false);
  BOARD_ToggleGreen(false);
  if (gSettings.backlightOnSquelch == BL_SQL_OPEN)
    BACKLIGHT_TurnOff();
  LogC(LOG_C_YELLOW, "[RADIO] Audio MUTED (tail/sq-)");
}

// Восстановить аудио (сигнал вернулся во время tail hold)
void RADIO_UnmuteAudioNow(RadioState *state) {
  ExtendedVFOContext *ev = &state->vfos[state->active_vfo_index];
  if (ev->is_open)
    return; // уже открыт
  ev->is_open = true;
  RXSW_SwitchTo(&state->rx_switch, &ev->context, true);
  BOARD_ToggleGreen(true);
  if (gSettings.backlightOnSquelch != BL_SQL_OFF)
    BACKLIGHT_TurnOn();
  LogC(LOG_C_YELLOW, "[RADIO] Audio RESTORED (sq+ during tail)");
}
