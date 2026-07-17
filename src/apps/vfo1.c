#include "vfo1.h"
#include "../dcs.h"
#include "../driver/gpio.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/analysermenu.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/numnav.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../helper/vfomenu.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/chlist.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/lootlist.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"

static char String[16];
static bool gShowFreqScale = false;

static const char *graphMeasurementNames[] = {
    [GRAPH_RSSI] = "RSSI",     //
    [GRAPH_NOISE] = "Noise",   //
    [GRAPH_GLITCH] = "Glitch", //
    [GRAPH_SNR] = "SNR",       //
    [GRAPH_TX] = "TX amp",     //
};

static void updateBand(void) {
  uint32_t f = RADIO_GetParam(ctx, PARAM_FREQUENCY);
  // if (gCurrentBand.start == 0 && !BANDS_InRange(f, &gCurrentBand)) {
  gCurrentBand = BANDS_ByFrequency(f);
  // }
}

static void setChannel(uint16_t v) {
  RADIO_LoadChannelToVFO(gRadioState, RADIO_GetCurrentVFONumber(gRadioState),
                         v);
}

static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
  updateBand();
}

void VFO1_init(void) {
  gLastActiveLoot = NULL;
  // CHANNELS_LoadScanlist(TYPE_FILTER_CH, gSettings.currentScanlist);
  if (vfo->mode == MODE_CHANNEL) {
    setChannel(vfo->channel_index);
  }

  updateBand();

  SCAN_SetMode(SCAN_MODE_SINGLE);
}

void VFO1_update(void) {}

static bool handleNumNav(KEY_Code_t key) {
  if (gIsNumNavInput) {
    NUMNAV_Input(key);
    return true;
  }

  if (key <= KEY_9) {
    NUMNAV_Init(vfo->channel_index, 0, 4096 - 1);
    gNumNavCallback = setChannel;
    return false;
  }

  return false;
}

static bool handleFrequencyChange(KEY_Code_t key) {
  if (key != KEY_UP && key != KEY_DOWN)
    return false;

  if (vfo->mode == MODE_CHANNEL) {
    RADIO_NextChannel((key == KEY_UP) ^ gSettings.invertButtons);
  } else {
    RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
    RADIO_IncDecParam(ctx, PARAM_FREQUENCY,
                      (key == KEY_UP) ^ gSettings.invertButtons, true);
  }

  updateBand();
  return true;
}

static bool handleSSBFineTune(KEY_Code_t key) {
  if (ctx->radio_type != RADIO_SI4732 || !RADIO_IsSSB(ctx))
    return false;
  if (key != KEY_SIDE1 && key != KEY_SIDE2)
    return false;

  RADIO_AdjustParam(ctx, PARAM_FREQUENCY, key == KEY_SIDE1 ? 5 : -5, true);
  // TODO: SAVE
  return true;
}

static bool handleLongPress(KEY_Code_t key) {
  uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);

  switch (key) {
  case KEY_1:
    CHLIST_init();
    gChlistActive = true;
    return true;

  case KEY_2:
    if (gCurrentApp == APP_VFO1) {
      gSettings.iAmPro = !gSettings.iAmPro;
      SETTINGS_Save();
      return true;
    }
    return false;

  case KEY_3:
    RADIO_SaveCurrentVFO(gRadioState);
    RADIO_ToggleVFOMode(gRadioState, vfoN);
    updateBand();
    return true;

  case KEY_4:
    gShowAllRSSI = !gShowAllRSSI;
    return true;

  case KEY_5:
    RADIO_ToggleMultiwatch(gRadioState, !gRadioState->multiwatch_enabled);
    return true;

  case KEY_6:
    RADIO_IncDecParam(ctx, PARAM_POWER, true, true);
    return true;

  case KEY_7:
    RADIO_IncDecParam(ctx, PARAM_STEP, true, true);
    return true;

  case KEY_8:
    gShowFreqScale = !gShowFreqScale;
    return true;

  case KEY_0:
    ANALYSERMENU_Toggle();
    return true;

  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_MODULATION, true, true);
    return true;

  case KEY_SIDE1:
  case KEY_SIDE2:
    SP_NextGraphUnit(key == KEY_SIDE1);
    return true;

  default:
    return false;
  }
}

static bool handleRelease(KEY_Code_t key, Key_State_t state) {
  uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);

  if (gSettings.iAmPro) {
    switch (key) {
    case KEY_1:
    case KEY_7:
      // sq
      RADIO_IncDecParam(ctx, PARAM_SQUELCH_VALUE, key == KEY_1, true);
      return true;
    case KEY_2:
    case KEY_8:
      // sq
      RADIO_IncDecParam(ctx, PARAM_BANDWIDTH, key == KEY_2, true);
      return true;
    case KEY_3:
    case KEY_9:
      // step
      RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, true);
      return true;
    case KEY_6:
      // mod
      RADIO_IncDecParam(ctx, PARAM_MODULATION, true, true);
      return true;
    case KEY_5:
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
      FINPUT_Show(tuneTo);
      return true;
    default:
      break;
    }
  }

  switch (key) {
  case KEY_0:
  case KEY_1:
  case KEY_2:
  case KEY_3:
  case KEY_4:
  case KEY_5:
  case KEY_6:
  case KEY_7:
  case KEY_8:
  case KEY_9:
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(tuneTo);
    FINPUT_key(key, state);
    return true;

  case KEY_STAR:
    LOOTLIST_init();
    gLootlistActive = true;
    return true;

  case KEY_SIDE1:
    gMonitorMode = !gMonitorMode;
    return true;

  case KEY_SIDE2:
    GPIO_TogglePin(GPIO_PIN_FLASHLIGHT);
    return true;

  case KEY_EXIT:
    if (gMonitorMode) {
      gMonitorMode = false;
      return true;
    }

    if (!APPS_exit()) {
      RADIO_SaveCurrentVFO(gRadioState);
      RADIO_SwitchVFO(gRadioState,
                      IncDecU(vfoN, 0, gRadioState->num_vfos, true));
      updateBand();
    }

    return true;

  default:
    return false;
  }
}

static GraphMeasurement _graphMeasurement;

bool VFO1_key(KEY_Code_t key, Key_State_t state) {
  // return false;
  if (REGSMENU_Key(key, state)) {
    return true;
  }

  if (ANALYSERMENU_Key(key, state)) {
    return true;
  }

  if (VFOMENU_Key(key, state)) {
    return true;
  }

  // Обработка NUM NAV в режиме канала
  if (state == KEY_RELEASED && vfo->mode == MODE_CHANNEL && !gIsNumNavInput) {
    if (handleNumNav(key)) {
      return true;
    }
  }

  // PTT
  if (key == KEY_PTT && !gIsNumNavInput) {
    const bool isTx = state != KEY_RELEASED;
    if (isTx) {
      _graphMeasurement = graphMeasurement;
      graphMeasurement = GRAPH_TX;
    } else {
      graphMeasurement = _graphMeasurement;
    }

    RADIO_ToggleTX(ctx, isTx);

    return true;
  }

  // Обработка нажатий и удержаний
  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (handleFrequencyChange(key))
      return true;
    if (handleSSBFineTune(key))
      return true;
  }

  // Длинные нажатия
  if (state == KEY_LONG_PRESSED) {
    return handleLongPress(key);
  }

  // Отпускания
  if (state == KEY_RELEASED) {
    return handleRelease(key, state);
  }

  return false;
}

static void renderTxRxState(uint8_t y, bool isTx) {
  if (isTx) {
    if (ctx->tx_state.is_active) {
      PrintMediumEx(0, 21, POS_L, C_FILL, "TX");
    } else {
      PrintMediumEx(LCD_XCENTER, y, POS_C, C_FILL, "%s",
                        RADIO_GetParamValueString(ctx, PARAM_TX_STATE));
    }
  } else if (vfo->msm.open) {
    PrintMediumEx(0, 21, POS_L, C_FILL, "RX");
  }
}

static void renderChannelName(uint8_t y, uint16_t channel) {
  uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);
  FillRect(0, y - 14, 30, 7, C_FILL);
  PrintSmallEx(15, y - 9, POS_C, C_INVERT, "VFO %u/%u", vfoN + 1,
               gRadioState->num_vfos);
  if (gRadioState->vfos[vfoN].mode == MODE_CHANNEL) {
    PrintSmallEx(32, y - 9, POS_L, C_FILL, "MR %03u", channel);
  }
  UI_Scanlists(LCD_WIDTH - 25, y - 13, gSettings.currentScanlist);
}

static void renderStatusLine(void) {
  if (gIsNumNavInput) {
    return;
  }
  if (!gSettings.mWatch || vfo->is_open) {
    STATUSLINE_RenderRadioSettings();
  } else {
    STATUSLINE_SetText("Radio: %s",
                       RADIO_GetParamValueString(ctx, PARAM_RADIO));
  }
}

static void renderBandInfo(uint8_t BASE) {
  if (vfo->mode == MODE_CHANNEL) {
    PrintMediumEx(LCD_XCENTER, BASE - 16, POS_C, C_FILL, "%s", ctx->name);
  } else {
    const char *format = (gCurrentBand.detached) ? "*%s" : "%s:%u";

    if (gCurrentBand.detached) {
      PrintSmallEx(32, 12, POS_L, C_FILL, format, gCurrentBand.name);
    } else {
      uint32_t channel = BANDS_GetChannel(&gCurrentBand, ctx->frequency) + 1;
      PrintSmallEx(32, 12, POS_L, C_FILL, format, gCurrentBand.name, channel);
    }
  }
}

static void renderCodes(uint8_t BASE) {
  if (ctx->code.type) {
    PrintSmallEx(0, BASE - 6, POS_L, C_FILL, "R%s",
                 RADIO_GetParamValueString(ctx, PARAM_RX_CODE));
  }

  if (ctx->tx_state.code.type) {
    PrintSmallEx(0, BASE, POS_L, C_FILL, "T%s",
                 RADIO_GetParamValueString(ctx, PARAM_TX_CODE));
  }
}

static void renderExtraInfo(uint8_t BASE) {
  uint32_t txF = RADIO_GetParam(ctx, PARAM_TX_FREQUENCY_FACT);
  uint32_t rxF = RADIO_GetParam(ctx, PARAM_FREQUENCY);
  bool isTxFDifferent = (txF != rxF);

  // PrintSmallEx(14, 21, POS_L, C_FILL, "%u", BK4819_ReadRegister(0x6D));
  int16_t afcVal = BK4819_GetAFCValue();
  if (afcVal != 0) {
    PrintSmallEx(14, 21, POS_L, C_FILL, "%+d", BK4819_GetAFCValue());
  }

  if (ctx->tx_state.is_active) {
    PrintSmallEx(14, 27, POS_L, C_FILL, "%u", BK4819_GetAfTxRx());
  }

  if (gSettings.iAmPro) {
    PrintSmallEx(LCD_WIDTH - 1, BASE + 8 + 6, POS_R, C_FILL, "PRO");
  }

  if (isTxFDifferent && ctx->upconverter == 0) {
    PrintSmallEx(LCD_XCENTER, BASE + 6, POS_C, C_FILL, "TX: %s",
                 RADIO_GetParamValueString(ctx, PARAM_TX_FREQUENCY_FACT));
  } else if (ctx->upconverter != 0) {
    PrintSmallEx(LCD_XCENTER, BASE + 6, POS_C, C_FILL, "RX: %s",
                 RADIO_GetParamValueString(ctx, PARAM_FREQUENCY_FACT));
  }
}

static void renderLootInfo(void) {
  if (!gLastActiveLoot)
    return;

  const uint32_t ago = LOOT_SecondsAgo(gLastActiveLoot);

  if (gLastActiveLoot->code != 255) {
    if (gLastActiveLoot->isCd) {
      PrintRTXCode(String, CODE_TYPE_DIGITAL, gLastActiveLoot->code);
    } else {
      PrintRTXCode(String, CODE_TYPE_CONTINUOUS_TONE, gLastActiveLoot->code);
    }

    PrintSmallEx(0, LCD_HEIGHT - 1, POS_L, C_FILL, "%s", String);
  }

  UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, LCD_HEIGHT - 1, POS_C);

  if (ago) {
    PrintSmallEx(LCD_WIDTH, LCD_HEIGHT - 1, POS_R, C_FILL, "%u:%02u", ago / 60,
                 ago % 60);
  }

  if (gRadioState->multiwatch_enabled) {
    PrintMediumEx(LCD_XCENTER, LCD_HEIGHT - 9, POS_C, C_FILL, "M");
  }
}

static uint8_t scaleF2X(uint32_t f, uint32_t fs, uint32_t fe) {
  if (f <= fs)
    return 0;
  if (f >= fe)
    return LCD_WIDTH - 1;
  return (uint8_t)(((uint64_t)(f - fs) * (LCD_WIDTH - 1)) / (fe - fs));
}

static void drawCenterArrow(uint8_t axisY) {
  const uint8_t cx = LCD_XCENTER - 1;
  DrawVLine(cx, axisY + 1, 2, C_FILL);
  FillRect(cx - 1, axisY + 3, 3, 1, C_FILL);
  FillRect(cx - 2, axisY + 4, 5, 1, C_FILL);
}

static void drawLootMarker(uint8_t x, uint8_t axisY, bool active) {
  const uint8_t h = 3;
  DrawVLine(x, axisY - h, h, C_FILL);
  PutPixel(x - 1, axisY - h, C_FILL);
  PutPixel(x + 1, axisY - h, C_FILL);
}

static void drawVisibleLoot(uint32_t fs, uint32_t fe, uint8_t axisY) {
  for (uint16_t i = 0; i < LOOT_Size(); ++i) {
    const Loot *loot = LOOT_Item(i);
    if (loot->f < fs || loot->f > fe)
      continue;

    const uint8_t x = scaleF2X(loot->f, fs, fe);
    const bool isActive = (loot == gLastActiveLoot);
    drawLootMarker(x, axisY - 3, true);
  }
}

static void renderFrequencyScale(uint8_t BASE, uint32_t centerF) {
  const uint8_t y = BASE + 8;
  const uint8_t h = LCD_HEIGHT - y;
  const uint8_t axisY = y + h - 3;
  const uint32_t step = StepFrequencyTable[ctx->step];
  const uint32_t span = step * 20;
  const uint32_t half = span / 2;
  const uint32_t fs = centerF > half ? centerF - half : 0;
  const uint32_t fe = centerF + half;
  uint32_t major = step * 5;

  FillRect(0, y, LCD_WIDTH, h, C_CLEAR);
  PrintSmallEx(0, y + 5, POS_L, C_FILL, "SCL");
  DrawHLine(0, axisY, LCD_WIDTH, C_FILL);

  if (major < KHZ)
    major = KHZ;

  uint32_t first = fs - (fs % step);
  for (uint32_t ff = first; ff <= fe; ff += step) {
    uint8_t x = scaleF2X(ff, fs, fe);
    const bool isMajor = (ff % major) == 0;
    const uint8_t th = isMajor ? 5 : 2;
    DrawVLine(x, axisY - th + 1, th, C_FILL);
  }

  // DrawVLine(LCD_XCENTER, y, axisY - y + 1, C_INVERT);
  /* PrintSmallEx(LCD_XCENTER, y + 5, POS_C, C_FILL, "%u.%05u", centerF / MHZ,
               centerF % MHZ); */
  drawVisibleLoot(fs, fe, axisY);
  drawCenterArrow(axisY);
}

static void renderMonitorMode(uint8_t BASE) {
  SPECTRUM_Y = BASE + 2;
  SPECTRUM_H = LCD_HEIGHT - SPECTRUM_Y;

  if (gSettings.showLevelInVFO || ctx->tx_state.is_active) {
    static const struct {
      uint16_t min;
      uint16_t max;
    } graphRanges[] = {
        [GRAPH_RSSI] = {RSSI_MIN, RSSI_MAX},
        [GRAPH_NOISE] = {0, 255},
        [GRAPH_GLITCH] = {0, 255},
        [GRAPH_SNR] = {0, 30},
        [GRAPH_TX] = {0, 8192},
        [GRAPH_COUNT] = {RSSI_MIN, RSSI_MAX},
    };

    SP_RenderGraph(graphRanges[graphMeasurement].min,
                   graphRanges[graphMeasurement].max);
    PrintSmallEx(0, SPECTRUM_Y + 5, POS_L, C_FILL, "%s %+3u",
                 graphMeasurementNames[graphMeasurement],
                 SP_GetLastGraphValue());

    if (ctx->tx_state.is_active) {
      FillRect(0, LCD_HEIGHT - 8, 24, 8, C_CLEAR);
      PrintMediumEx(1, LCD_HEIGHT - 1, POS_L, C_FILL, "%u%c",
                    ctx->tx_state.power_level,
                    ctx->tx_state.pa_enabled ? '+' : '-');
    }
  } else {
    UI_RSSIBar(BASE + 1 + 6);
  }
}

void VFO1_render(void) {
  const uint8_t BASE = 39;

  renderStatusLine();

  uint32_t f = RADIO_GetParam(
      ctx, ctx->tx_state.is_active ? PARAM_TX_FREQUENCY_FACT : PARAM_FREQUENCY);
  const char *mod = RADIO_GetParamValueString(ctx, PARAM_MODULATION);

  renderBandInfo(BASE);
  renderTxRxState(BASE - 4,
                  ctx->tx_state.is_active || ctx->tx_state.last_error);

  if (!ctx->tx_state.last_error) {
    UI_BigFrequency(BASE, f);

    PrintMediumEx(LCD_WIDTH - 1, BASE - 9, POS_R, C_FILL, mod);
    renderChannelName(21, vfo->channel_index);

    const uint32_t step = StepFrequencyTable[ctx->step];
    PrintSmallEx(LCD_WIDTH, BASE + 6, POS_R, C_FILL, "%d.%02d", step / KHZ,
                 step % KHZ);

    renderCodes(BASE);
    renderExtraInfo(BASE);
    renderLootInfo();

    if (gMonitorMode || ctx->tx_state.is_active) {
      renderMonitorMode(BASE);
    } else if (gShowFreqScale) {
      renderFrequencyScale(BASE, f);
    } else {
      if (vfo->is_open || gSettings.alwaysRssi) {
        UI_RSSIBar(BASE + 1 + 6);
      }
    }
  }

  if (ctx->tx_state.is_active) {
    UI_TxBar(BASE + 1);
  }

  REGSMENU_Draw();
  ANALYSERMENU_Draw();
  VFOMENU_Draw();
}
