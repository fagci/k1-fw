#include "fc.h"
#include "../dcs.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdint.h>

#define FM_BAND_LOW (88 * MHZ)
#define FM_BAND_HIGH (108 * MHZ)
#define FC_HZ_LOW 290u
#define FC_HZ_HIGH 0x580

// Выдержка (мс) после перестройки перед опросом шумодава
#define FC_SQUELCH_SETTLE_MS 80

static const uint8_t REQUIRED_FREQUENCY_HITS = 2;
static const uint8_t FILTER_SWITCH_INTERVAL = REQUIRED_FREQUENCY_HITS;

static const char *FILTER_NAMES[] = {
    [FILTER_OFF] = "ALL",
    [FILTER_VHF] = "Very HF",
    [FILTER_UHF] = "Ultra HF",
};

static Filter filter = FILTER_VHF;
static bool bandAutoSwitch = true;
static bool roundToStep = true; // округлять до шага по умолчанию
static uint32_t bound;
static bool isScanning = true;
static uint32_t currentFrequency = 0;
static uint32_t lastDetectedFrequency = 0;
static uint8_t frequencyHits = 0;
static uint8_t filterSwitchCounter = 0;
static uint16_t hz = FC_HZ_LOW;
static uint32_t fcTimeout;
static uint32_t fcListenTimer;

// Отложенный перезапуск сканирования (вместо sync disable+enable в update)
static bool pendingScanRestart = false;
static uint32_t scanRestartAt = 0;
#define SCAN_RESTART_DELAY_MS 2

// ─── Внутренние ──────────────────────────────────────────────────────────────

static void enableScan(void) {
  // Правильно инициализируем частоту для сканера — REG_51 в нужном состоянии
  uint32_t centerF = (filter == FILTER_VHF)
                         ? 14500000u  // 145 МГц, середина VHF (в 10 Гц)
                         : 43300000u; // 433 МГц, середина UHF (в 10 Гц)
  BK4819_SetScanFrequency(
      centerF); // устанавливает частоту + REG_51 + RX_TurnOn

  BK4819_EnableFrequencyScanEx2(gSettings.fcTime, hz);
  isScanning = true;
  pendingScanRestart = false;
}
/* static void enableScan(void) {
  BK4819_EnableFrequencyScanEx2(gSettings.fcTime, hz);
  isScanning = true;
  pendingScanRestart = false;
} */

static void disableScan(void) {
  BK4819_DisableFrequencyScan();
  BK4819_RX_TurnOn();
  isScanning = false;
}

static void switchFilter(void) {
  filter = (filter == FILTER_VHF) ? FILTER_UHF : FILTER_VHF;
  BK4819_SelectFilterEx(filter);
  filterSwitchCounter = 0;
  gRedrawScreen = true;
}

static bool isFrequencyValid(uint32_t freq) {
  if (freq >= FM_BAND_LOW && freq <= FM_BAND_HIGH)
    return false;
  if (filter == FILTER_VHF && freq >= bound)
    return false;
  if (filter == FILTER_UHF && freq < bound)
    return false;
  return true;
}

static uint32_t applyRounding(uint32_t freq) {
  if (!roundToStep)
    return freq;
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  if (step == 0)
    return freq;
  return (freq / step) * step;
}

static void scheduleRestartScan(void) {
  pendingScanRestart = true;
  scanRestartAt = Now() + SCAN_RESTART_DELAY_MS;
}

static uint32_t lastScreenUpdate = 0;
#define SCREEN_UPDATE_INTERVAL_MS 500  // Не обновлять экран чаще чем раз в 500мс

// Отслеживаем состояние шумодава для перерисовки только при изменении
static bool lastSquelchOpen = false;

// ─── Обработка результата сканирования ───────────────────────────────────────

static void handleScanResult(void) {
  if (!BK4819_GetFrequencyScanResult(&currentFrequency))
    return;

  // Не перерисовываем каждый раз — это создаёт SPI помехи!
  if (Now() - lastScreenUpdate >= SCREEN_UPDATE_INTERVAL_MS) {
    gRedrawScreen = true;
    lastScreenUpdate = Now();
  }

  if (bandAutoSwitch)
    filterSwitchCounter++;

  if (isFrequencyValid(currentFrequency) &&
      DeltaF(currentFrequency, lastDetectedFrequency) < 300) {
    frequencyHits++;
  } else {
    frequencyHits = 1;
  }

  lastDetectedFrequency = currentFrequency;

  if (frequencyHits >= REQUIRED_FREQUENCY_HITS) {
    uint32_t f = applyRounding(currentFrequency);
    disableScan();
    BK4819_SetAFC(0);  // Явно отключаем AFC до перестройки

    RADIO_SetParam(ctx, PARAM_FREQUENCY, f, false);
    RADIO_ApplySettings(ctx);
    BK4819_SetAFC(0);  // Гарантируем отключение после ApplySettings

    frequencyHits = 0;
    // Ждём оседания до первой проверки шумодава
    fcListenTimer = Now() + FC_SQUELCH_SETTLE_MS;
  } else {
    disableScan();
    scheduleRestartScan();
  }
}

// ─── Публичный API ───────────────────────────────────────────────────────────

void FC_init(void) {
  bound = SETTINGS_GetFilterBound();
  BK4819_SelectFilterEx(filter);
  enableScan();
  frequencyHits = 0;
  filterSwitchCounter = 0;
  pendingScanRestart = false;
  lastScreenUpdate = 0;
  lastSquelchOpen = false;
  gSuppressDisplayUpdates = false; // Разрешаем обновления при старте
  // Гарантируем, что первая проверка шумодава не сработает мгновенно
  fcListenTimer = Now() + FC_SQUELCH_SETTLE_MS;
  SCAN_SetMode(SCAN_MODE_NONE);
}

void FC_deinit(void) { disableScan(); }

void FC_update(void) {
  if (!CheckTimeout(&fcTimeout))
    return;

  RADIO_UpdateMultiwatch(gRadioState);
  RADIO_CheckAndSaveVFO(gRadioState);

  // Отложенный перезапуск сканера (неблокирующий)
  if (pendingScanRestart) {
    if (Now() >= scanRestartAt) {
      enableScan();
      SetTimeout(&fcTimeout, 200 << gSettings.fcTime);
    } else {
      SetTimeout(&fcTimeout, SCAN_RESTART_DELAY_MS);
    }
    return;
  }

  ExtendedVFOContext *vfo = RADIO_GetCurrentVFO(gRadioState);

  if (isScanning) {
    handleScanResult();

    if (bandAutoSwitch && filterSwitchCounter >= FILTER_SWITCH_INTERVAL)
      switchFilter();

    SetTimeout(&fcTimeout, 200 << gSettings.fcTime);
  } else {
    // Ждём оседания радио перед проверкой шумодава
    if (Now() < fcListenTimer) {
      SetTimeout(&fcTimeout, fcListenTimer - Now() + 1);
      return;
    }

    RADIO_UpdateSquelch(gRadioState);

    // Подавляем обновления дисплея при открытом шумодаве — SPI создаёт помехи
    gSuppressDisplayUpdates = vfo->is_open;

    // Перерисовываем только при изменении состояния шумодава
    if (vfo->is_open != lastSquelchOpen) {
      gRedrawScreen = true;
      lastSquelchOpen = vfo->is_open;
    }

    if (!vfo->is_open) {
      enableScan();
    }

    // Следующий опрос через SQL_DELAY
    fcListenTimer = Now() + SQL_DELAY;
    SetTimeout(&fcTimeout, SQL_DELAY);
  }
}

bool FC_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state))
    return true;

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (key == KEY_5) {
      hz = (hz == FC_HZ_LOW) ? FC_HZ_HIGH : FC_HZ_LOW;
      gRedrawScreen = true;
    }
  }

  if (state != KEY_RELEASED)
    return false;

  switch (key) {
  case KEY_1:
  case KEY_7:
    gSettings.fcTime = IncDecU(gSettings.fcTime, 0, 3 + 1, key == KEY_1);
    SETTINGS_DelayedSave();
    FC_init();
    break;
  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_SQUELCH_VALUE, key == KEY_3, true);
    RADIO_ApplySettings(ctx);
    break;
  case KEY_4:
    roundToStep = !roundToStep;
    gRedrawScreen = true;
    return true;
  case KEY_STAR:
    return true;
  case KEY_SIDE1:
    LOOT_BlacklistLast();
    return true;
  case KEY_SIDE2:
    LOOT_WhitelistLast();
    return true;
  case KEY_F:
    switchFilter();
    return true;
  case KEY_6:
    bandAutoSwitch = !bandAutoSwitch;
    gRedrawScreen = true;
    return true;
  case KEY_PTT:
    if (gLastActiveLoot) {
      FC_deinit();
      RADIO_SetParam(ctx, PARAM_FREQUENCY, gLastActiveLoot->f, true);
      APPS_run(APP_VFO1);
    }
    return true;
  default:
    break;
  }

  return false;
}

void FC_render(void) {
  const uint8_t BASE = 40;

  ExtendedVFOContext *vfo = RADIO_GetCurrentVFO(gRadioState);

  STATUSLINE_RenderRadioSettings();

  // Строка параметров (y=12)
  PrintSmallEx(0, 12, POS_L, C_FILL, "%s %ums HZ%u", FILTER_NAMES[filter],
               200 << gSettings.fcTime, hz);
  PrintSmallEx(LCD_WIDTH, 12, POS_R, C_FILL, "SQ%u%s%s",
               RADIO_GetParam(ctx, PARAM_SQUELCH_VALUE),
               roundToStep ? " R" : "", bandAutoSwitch ? " A" : "");

  // Статус сканера (y=22)
  if (isScanning || pendingScanRestart) {
    PrintSmallEx(LCD_XCENTER, 22, POS_C, C_FILL, "SCAN");
  } else if (vfo->is_open) {
    PrintSmallEx(LCD_XCENTER, 22, POS_C, C_FILL, "RX");
  } else {
    PrintSmallEx(LCD_XCENTER, 22, POS_C, C_FILL, "...");
  }

  // Большая частота
  UI_BigFrequency(BASE, currentFrequency);

  // Последний активный сигнал
  if (gLastActiveLoot) {
    char string[16];
    UI_DrawLoot(gLastActiveLoot, LCD_WIDTH, BASE + 8, POS_R);
    if (gLastActiveLoot->code != 255) {
      DCS_CodeType_t type =
          gLastActiveLoot->isCd ? CODE_TYPE_DIGITAL : CODE_TYPE_CONTINUOUS_TONE;
      PrintRTXCode(string, type, gLastActiveLoot->code);
      PrintSmallEx(LCD_WIDTH, BASE + 8 + 6, POS_R, C_FILL, "%s", string);
    }

    const uint32_t ago = LOOT_SecondsAgo(gLastActiveLoot);
    if (ago) {
      PrintSmallEx(0, BASE + 8, POS_L, C_FILL, "%u:%02u", ago / 60, ago % 60);
    }
  }

  // RSSI-бар при открытом шумодаве
  if (!isScanning && vfo->is_open) {
    UI_RSSIBar(LCD_HEIGHT - 6);
  }

  REGSMENU_Draw();
}
