#include "analyser.h"
#include "../driver/hrtime.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/analysermenu.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../helper/storage.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "../ui/toast.h"
#include "apps.h"
#include <stdbool.h>
#include <string.h>

// ── Constants ──────────────────────────────────────────────────────────────

#define ANALYSER_SETTINGS_MAGIC 0xA51Eu // bump при смене структуры
#define SQ_LEVEL_INVALID 0xFF
#define CURSOR_INFO_TIMEOUT_MS 2000
#define SETTINGS_SAVE_DEBOUNCE_MS 1000
#define DELAY_BLOCKING_MAX_US 10000 // ≤10мс — блокирующая задержка
#define DELAY_DEFAULT_US 2200
#define DELAY_MAX_US 90000
#define DEFAULT_RANGE_START 43307500u

// dBm limits
#define DBM_MIN_LO -140
#define DBM_MIN_HI 9
#define DBM_MAX_LO -139
#define DBM_MAX_HI 10

// Layout
#define SPLIT_GAP 4
#define SPECTRUM_H_FULL 44
#define SPECTRUM_H_SPLIT 24 // под peaks/listen/RSSI bar

// ── Persisted settings ─────────────────────────────────────────────────────

typedef struct {
  uint16_t magic;
  int16_t dbMin;
  int16_t dbMax;
  uint32_t delay;
  uint32_t lastRangeStart;
  uint32_t lastRangeEnd;
  uint8_t mode;
  uint8_t precise;
  uint8_t _pad[2];
} AnalyserSettings;

static AnalyserSettings aSettings = {
    .magic = ANALYSER_SETTINGS_MAGIC,
    .dbMin = -115,
    .dbMax = -40,
    .delay = DELAY_DEFAULT_US,
    .lastRangeStart = 0,
    .lastRangeEnd = 0,
    .mode = 0,
};

static uint32_t analyserSaveTime;
static bool settingsDirty;

static void analyserSettingsLoad(void) {
  AnalyserSettings loaded = {0};
  STORAGE_LOAD("analyser.set", 0, &loaded);
  if (loaded.magic == ANALYSER_SETTINGS_MAGIC) {
    aSettings = loaded;
  }
}

static void analyserSettingsSave(void) {
  aSettings.magic = ANALYSER_SETTINGS_MAGIC;
  STORAGE_SAVE("analyser.set", 0, &aSettings);
}

// Каждое изменение перезапускает таймер — настоящий debounce
static void markSettingsDirty(void) {
  settingsDirty = true;
  analyserSaveTime = Now() + SETTINGS_SAVE_DEBOUNCE_MS;
}

// ── Analyser state ─────────────────────────────────────────────────────────

static Band range;
static uint32_t targetF;
static uint32_t
    scanF; // курсор сканера, отдельно от msm->f (где реально мерили)
static uint32_t delay = DELAY_DEFAULT_US;
static bool still;
static bool listen;
static Measurement *msm;
static uint32_t cursorRangeTimeout = 0;

// Неблокирующее ожидание settle при delay > DELAY_BLOCKING_MAX_US
static uint32_t scanWaitUntil;
static bool scanAwaitingSettle;

// Маркеры A/B. Auto = копируется из peaks[0/1] каждый свип.
// Manual = зафиксирован пользователем через long KEY_5, не обновляется.
typedef struct {
  uint32_t f;
  bool manual;
} Marker;
static Marker markerA;
static Marker markerB;

// Squelch tuner
static SQL sq;
static bool showSqTuner;

typedef enum {
  SQ_EDIT_RSSI,
  SQ_EDIT_NOISE,
  SQ_EDIT_GLITCH,
} SqEditParam;

static SqEditParam sqEditParam = SQ_EDIT_RSSI;
static uint8_t sqEditLevel;
static uint8_t lastSquelchLevel = SQ_LEVEL_INVALID;

// dBm tuner (редактирует верхнюю/нижнюю границу шкалы Y)
static bool showDbmTuner;
static bool preciseMode;

// glitch зашкаливает при precise + delay < 25мс
static bool glitchDisabled(void) { return preciseMode && delay < 25000; }

// Режимы анализатора (переключаются short KEY_SIDE2, сохраняются)
typedef enum {
  MODE_SCAN, // спектр + always-listen (шумодав форсированно открыт)
  MODE_PEAKS, // спектр + таблица пиков с R/N/G
  MODE_COUNT,
} AnalyserMode;

static AnalyserMode analyserMode = MODE_SCAN;

// Top-N пиков в режиме PEAKS
#define PEAKS_N 3
#define PEAKS_MIN_GAP 8 // мин. расстояние между пиками, пикс

typedef struct {
  uint8_t x;
  uint16_t rssi;
} PeakEntry;

static PeakEntry peaks[PEAKS_N];
static uint8_t peaksFound;

// ── Delay helpers ──────────────────────────────────────────────────────────

static uint32_t adjustDelay(int32_t inc) {
  uint32_t step;
  if (delay < 3000) {
    step = 100;
  } else if (delay < 10000) {
    step = 1000;
  } else {
    step = 5000;
  }
  int32_t newDelay =
      (int32_t)delay + (inc > 0 ? (int32_t)step : -(int32_t)step);
  if (newDelay < 0)
    return 0;
  if (newDelay > DELAY_MAX_US)
    return DELAY_MAX_US;
  return (uint32_t)newDelay;
}

static void setDelayUs(uint32_t newDelay) {
  if (newDelay == delay)
    return;
  delay = newDelay;
  scanAwaitingSettle = false; // старый scanWaitUntil невалиден
  markSettingsDirty();
}

// ── Squelch helpers ────────────────────────────────────────────────────────

static void sqApplyThresholds(SquelchPreset p) {
  sq.ro = p.ro;
  sq.no = p.no;
  sq.go = p.go;
  sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
  sq.nc = sq.no + SQ_HYSTERESIS;
  sq.gc = sq.go + SQ_HYSTERESIS;
}

// Отслеживаем последний режим, в котором применили шумодав. В SCAN форсируем
// {255,0,0} только при входе — иначе тюнер R/N/G не даст крутить значения.
static AnalyserMode lastSqMode = (AnalyserMode)0xFF;

static void applySquelchPreset(void) {
  if (analyserMode == MODE_SCAN) {
    if (lastSqMode != MODE_SCAN) {
      sqApplyThresholds((SquelchPreset){.ro = 255, .no = 0, .go = 0});
      lastSqMode = MODE_SCAN;
      lastSquelchLevel = SQ_LEVEL_INVALID;
    }
    return;
  }
  // Прочие режимы: пресет по уровню шумодава. При выходе из SCAN
  // (lastSqMode == MODE_SCAN) форсируем перечитывание.
  if (showSqTuner) // sq под контролем пользователя
    return;
  if (ctx->squelch.value != lastSquelchLevel || lastSqMode == MODE_SCAN) {
    lastSquelchLevel = ctx->squelch.value;
    sqEditLevel = ctx->squelch.value;
    sqApplyThresholds(GetSqlPreset(sqEditLevel, ctx->frequency));
    lastSqMode = analyserMode;
  }
}

// ── Peaks ──────────────────────────────────────────────────────────────────

// Жадно ищем топ-N пиков в rssiHistory с минимальным gap между ними.
// Вызывать после завершения свипа, пока visited ещё содержит актуальные данные.
static void updatePeaks(void) {
  peaksFound = 0;
  bool used[LCD_WIDTH] = {0};

  for (uint8_t p = 0; p < PEAKS_N; p++) {
    uint16_t best = 0;
    uint8_t bestX = 0xFF;
    for (uint8_t x = 0; x < LCD_WIDTH; x++) {
      if (used[x])
        continue;
      uint16_t r = SP_GetPointRSSI(x);
      if (r > best) {
        best = r;
        bestX = x;
      }
    }
    if (bestX == 0xFF || best == 0)
      break;

    peaks[p].x = bestX;
    peaks[p].rssi = best;
    peaksFound++;

    uint8_t lo = (bestX > PEAKS_MIN_GAP) ? bestX - PEAKS_MIN_GAP : 0;
    uint8_t hi = (bestX + PEAKS_MIN_GAP < LCD_WIDTH) ? bestX + PEAKS_MIN_GAP
                                                     : LCD_WIDTH - 1;
    for (uint8_t x = lo; x <= hi; x++)
      used[x] = true;
  }
}

// ── Mode switching ─────────────────────────────────────────────────────────

// Нужна ли уменьшенная высота спектра (для peaks / RSSI bar)
static bool needSplit(void) {
  return analyserMode == MODE_PEAKS || still || listen;
}

// Пересчитать SPECTRUM_H под текущее состояние. SP_Init сбросит буфер —
// вызывать только когда состояние действительно изменилось.
static void refreshSpectrumH(void) {
  uint8_t newH = needSplit() ? SPECTRUM_H_SPLIT : SPECTRUM_H_FULL;
  if (newH == SPECTRUM_H)
    return;
  SPECTRUM_H = newH;
  SP_Init(&range);
}

// Обновить auto-маркеры на топ-2 пика (вызывается после updatePeaks)
static void updateMarkersFromPeaks(void) {
  if (!markerA.manual)
    markerA.f = (peaksFound > 0) ? SP_X2F(peaks[0].x) : 0;
  if (!markerB.manual)
    markerB.f = (peaksFound > 1) ? SP_X2F(peaks[1].x) : 0;
}

static void cycleMode(void) {
  analyserMode = (analyserMode + 1) % MODE_COUNT;
  peaksFound = 0;
  // Auto-маркеры обнулим — в новом режиме они пересчитаются или
  // вовсе не актуальны (см. renderUserMarker). Manual остаются.
  if (!markerA.manual)
    markerA.f = 0;
  if (!markerB.manual)
    markerB.f = 0;
  markSettingsDirty();
  refreshSpectrumH();
  gRedrawScreen = true;
}

// ── Markers ────────────────────────────────────────────────────────────────

// FINPUT callback для long KEY_5 в scan-режимах — ставит ручной маркер
// в первый auto-слот.
static void onMarkerFreq(uint32_t fs, uint32_t fe) {
  (void)fe;
  if (!markerA.manual) {
    markerA.f = fs;
    markerA.manual = true;
  } else if (!markerB.manual) {
    markerB.f = fs;
    markerB.manual = true;
  } else {
    // Оба слота заняты — даём пользователю обратную связь
    TOAST_Push("Markers full, long KEY5 to reset");
  }
}

// FINPUT callback для long KEY_5 в STILL — переход на введённую частоту
static void onStillJumpFreq(uint32_t fs, uint32_t fe) {
  (void)fe;
  targetF = fs;
  msm->f = fs;
  scanF = fs;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, fs, false);
  RADIO_ApplySettings(ctx);
}

// Long KEY_5 в scan-режимах: если оба маркера manual → сброс в auto.
// Иначе — FINPUT для нового manual маркера.
static void handleMarkerInput(void) {
  if (markerA.manual && markerB.manual) {
    markerA.manual = false;
    markerB.manual = false;
    gRedrawScreen = true;
    return;
  }
  FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
  FINPUT_Show(onMarkerFreq);
}

// ── dBm tuner helpers ──────────────────────────────────────────────────────

static int16_t clampI16(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

// Применить новые min/max с clamp и гарантией min < max
static void applyDbmBounds(int16_t newMin, int16_t newMax, bool maxChanged) {
  newMin = clampI16(newMin, DBM_MIN_LO, DBM_MIN_HI);
  newMax = clampI16(newMax, DBM_MAX_LO, DBM_MAX_HI);
  if (newMax <= newMin) {
    // Двигаем тот, который пользователь НЕ крутил
    if (maxChanged)
      newMin = newMax - 1;
    else
      newMax = newMin + 1;
  }
  ANALYSERMENU_SetDbmMin(newMin);
  ANALYSERMENU_SetDbmMax(newMax);
  markSettingsDirty();
  gRedrawScreen = true;
}

// ── Save scheduler ─────────────────────────────────────────────────────────

void ANALYSER_UpdateSave(void) {
  if (!settingsDirty)
    return;
  if (Now() < analyserSaveTime)
    return;

  aSettings.dbMin = ANALYSERMENU_GetDbmMin();
  aSettings.dbMax = ANALYSERMENU_GetDbmMax();
  aSettings.delay = delay;
  aSettings.lastRangeStart = range.start;
  aSettings.lastRangeEnd = range.end;
  aSettings.mode = (uint8_t)analyserMode;
  aSettings.precise = preciseMode ? 1 : 0;
  analyserSettingsSave();
  settingsDirty = false;
}

// ── Auto-scale dBm ─────────────────────────────────────────────────────────
// Двойной тап KEY_6: переключает авто-уровень.

#define KEY6_DOUBLE_TAP_MS 300

static uint32_t key6PendingSingle;
static bool autoLevelMode = false;

// ── Range helpers ──────────────────────────────────────────────────────────

// Всё, что обнуляется при смене диапазона: пики, auto-маркеры,
// состояние settle и debounced save. Manual-маркеры сохраняются — они
// привязаны к абсолютной частоте, а не к позиции на экране.
static void onRangeChanged(void) {
  peaksFound = 0;
  if (!markerA.manual)
    markerA.f = 0;
  if (!markerB.manual)
    markerB.f = 0;
  scanAwaitingSettle = false;
  markSettingsDirty();
}

static void setRange(uint32_t fs, uint32_t fe) {
  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = fs;
  range.end = fe;
  msm->f = range.start;
  scanF = range.start;
  SP_Init(&range);
  BANDS_RangeClear();
  BANDS_RangePush(range);
  onRangeChanged();
}

// Выход из still: сбрасывает listen и monitor, чтобы они не утекали в SCAN
static void exitStill(void) {
  still = false;
  listen = false;
  gMonitorMode = false;
  refreshSpectrumH();
}

static void lockToPeak(void) {
  uint32_t f = SP_GetPeakF();
  if (!f)
    return;
  still = true;
  targetF = f;
  msm->f = f;
  scanF = f;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, false);
  RADIO_ApplySettings(ctx);
}

// ── Mode indicator ─────────────────────────────────────────────────────────

// Базовый режим — двухбуквенный ярлык (SC/PK/SL/HLD/LSN)
static const char *getModeStr(void) {
  if (listen)
    return "LSN";
  if (still)
    return "HLD";
  switch (analyserMode) {
  case MODE_PEAKS:
    return "PK";
  default:
    return "SC";
  }
}

// Активный оверлей (Q/D/A) — рисуется отдельно от режима, с инверсией
static char getOverlayChar(void) {
  if (showSqTuner)
    return 'Q';
  if (showDbmTuner)
    return 'D';
  if (autoLevelMode)
    return 'A';
  return 0;
}

// ── Key handlers ───────────────────────────────────────────────────────────

// dBm tuner: 2/8 крутят min, 3/9 крутят max. Autorepeat через
// LONG_PRESSED_CONT.
static bool dbmTunerKey(KEY_Code_t key, Key_State_t state) {
  int16_t dbMin = ANALYSERMENU_GetDbmMin();
  int16_t dbMax = ANALYSERMENU_GetDbmMax();

  switch (key) {
  case KEY_2:
    applyDbmBounds(dbMin + 1, dbMax, false);
    return true;
  case KEY_8:
    applyDbmBounds(dbMin - 1, dbMax, false);
    return true;
  case KEY_3:
    applyDbmBounds(dbMin, dbMax + 1, true);
    return true;
  case KEY_9:
    applyDbmBounds(dbMin, dbMax - 1, true);
    return true;
  default:
    break;
  }
  return false;
}

static bool sqTunerKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_1:
  case KEY_7:
    graphMeasurement = GRAPH_RSSI;
    sqEditParam = SQ_EDIT_RSSI;
    sq.ro = AdjustU(sq.ro, 0, 255, key == KEY_1 ? 1 : -1);
    sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
    return true;
  case KEY_2:
  case KEY_8:
    graphMeasurement = GRAPH_NOISE;
    sqEditParam = SQ_EDIT_NOISE;
    sq.no = AdjustU(sq.no, 0, 128, key == KEY_2 ? 1 : -1);
    sq.nc = sq.no + SQ_HYSTERESIS;
    return true;
  case KEY_3:
  case KEY_9: {
    if (glitchDisabled())
      return true;
    graphMeasurement = GRAPH_GLITCH;
    sqEditParam = SQ_EDIT_GLITCH;
    sq.go = AdjustU(sq.go, 0, 255, key == KEY_3 ? 1 : -1);
    sq.gc = sq.go + SQ_HYSTERESIS;
    return true;
  }
  case KEY_4:
  case KEY_6:
    setDelayUs(adjustDelay(key == KEY_4 ? 1 : -1));
    return true;
  case KEY_MENU:
    return true;
  default:
    break;
  }
  return false;
}

static bool stillModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_UP:
  case KEY_DOWN: {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    int32_t delta = (key == KEY_UP) ? (int32_t)step : -(int32_t)step;
    if (gSettings.invertButtons)
      delta = -delta;
    targetF += delta;
    msm->f = targetF;
    scanF = targetF;
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
    return true;
  }
  case KEY_4:
  case KEY_6: {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    int32_t delta = (key == KEY_4) ? (int32_t)step : -(int32_t)step;
    targetF += delta;
    msm->f = targetF;
    scanF = targetF;
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
    return true;
  }
  case KEY_SIDE1:
    if (state == KEY_LONG_PRESSED || state == KEY_LONG_PRESSED_CONT) {
      // listen: long=whitelist (как в scan); still без listen: blacklist
      if (listen) {
        LOOT_WhitelistLast();
      } else {
        LOOT_BlacklistLast();
      }
    } else {
      // listen: short=blacklist (как в scan); still без listen: monitor toggle
      if (listen) {
        LOOT_BlacklistLast();
      } else {
        gMonitorMode = !gMonitorMode;
      }
    }
    return true;
  case KEY_SIDE2:
    LOOT_WhitelistLast();
    return true;
  default:
    break;
  }
  return false;
}

static bool analyzerModeKey(KEY_Code_t key, Key_State_t state) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  if (state == KEY_LONG_PRESSED) {
    switch (key) {}
  }
  switch (key) {
  case KEY_UP:
  case KEY_DOWN: {
    bool moved = CUR_Move((key == KEY_UP) ^ gSettings.invertButtons);
    cursorRangeTimeout = Now() + CURSOR_INFO_TIMEOUT_MS;

    if (!moved) {
      bool up = (key == KEY_UP) ^ gSettings.invertButtons;
      uint32_t oldStart = range.start;

      if (up) {
        range.start += step;
        range.end += step;
      } else {
        if (range.start >= step) {
          range.start -= step;
          range.end -= step;
        } else {
          range.start = 0;
          range.end = StepFrequencyTable[range.step] * LCD_WIDTH;
        }
      }
      if (range.end > BK4819_F_MAX) {
        range.end = BK4819_F_MAX;
        range.start = range.end - StepFrequencyTable[range.step] * LCD_WIDTH;
      }

      // Инкрементальный shift применим только если сдвинулись ровно на ±step.
      // При clamp'е к BK4819_F_MAX / 0 содержимое колонок не соответствует
      // новым X — нужен полный SP_Init.
      int32_t delta = (int32_t)range.start - (int32_t)oldStart;
      if (delta == (int32_t)step) {
        SP_ShiftGraph(-1);
      } else if (delta == -(int32_t)step) {
        SP_ShiftGraph(1);
      } else {
        SP_Init(&range);
      }

      BANDS_RangePeek()->start = range.start;
      BANDS_RangePeek()->end = range.end;
      SP_Begin();
      onRangeChanged();
      gRedrawScreen = true;
    }
    return true;
  }

  case KEY_1:
  case KEY_7:
    setDelayUs(adjustDelay(key == KEY_1 ? 1 : -1));
    return true;

  case KEY_2: { // zoom in по выделению курсора
    Band zoomed = CUR_GetRange(BANDS_RangePeek(), step);
    zoomed.step = RADIO_GetParam(ctx, PARAM_STEP);
    BANDS_RangePush(zoomed);
    range = *BANDS_RangePeek();
    msm->f = range.start;
    scanF = range.start;
    SP_Init(&range);
    CUR_Reset();
    onRangeChanged();
    return true;
  }

  case KEY_8: // zoom out
    BANDS_RangePop();
    range = *BANDS_RangePeek();
    RADIO_SetParam(ctx, PARAM_STEP, range.step, true);
    msm->f = range.start;
    scanF = range.start;
    SP_Init(&range);
    CUR_Reset();
    onRangeChanged();
    return true;

  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    range.step = RADIO_GetParam(ctx, PARAM_STEP);
    BANDS_RangePeek()->step = range.step;
    SP_Init(&range);
    return true;

  case KEY_4:
    if (state == KEY_LONG_PRESSED) {
      if (gLastActiveLoot) {
        still = true;
        listen = true;
        targetF = msm->f = gLastActiveLoot->f;
        scanF = targetF;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
        refreshSpectrumH();
      }
    } else if (state == KEY_RELEASED) {
      if (!still) {
        // вход в still
        still = true;
        listen = true;
        // Если шумодав открыт — встаём на активную частоту, не на курсор
        // сканера
        targetF =
            (gLastActiveLoot && vfo->is_open) ? gLastActiveLoot->f : msm->f;
        msm->f = targetF;
        scanF = targetF;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
        refreshSpectrumH();
      } else {
        // выход: сбросить still, listen и monitor
        exitStill();
      }
    }
    return true;

  case KEY_6:
    if (state == KEY_LONG_PRESSED) {
      showDbmTuner = !showDbmTuner;
      if (showDbmTuner)
        showSqTuner = false;
      key6PendingSingle = 0;
      return true;
    }
    if (state == KEY_RELEASED) {
      uint32_t now = Now();
      if (key6PendingSingle && (now - key6PendingSingle) < KEY6_DOUBLE_TAP_MS) {
        autoLevelMode = !autoLevelMode;
        TOAST_Push(autoLevelMode ? "Auto level ON" : "Auto level OFF");
        key6PendingSingle = 0;
      } else {
        // Первый тап — откладываем lockToPeak до истечения double-tap окна
        key6PendingSingle = now ? now : 1;
      }
    }
    return true;

  case KEY_SIDE1:
    if (state == KEY_LONG_PRESSED) {
      LOOT_WhitelistLast();
      return true;
    }
    if (state == KEY_RELEASED) {
      LOOT_BlacklistLast();
    }
    return true;
  case KEY_SIDE2:
    if (state == KEY_RELEASED) {
      cycleMode();
    } else if (state == KEY_LONG_PRESSED) {
      preciseMode = !preciseMode;
      markSettingsDirty();
      TOAST_Push(preciseMode ? "Precise ON" : "Precise OFF");
    }
    return true;
  case KEY_STAR:
    LOOTLIST_init();
    gLootlistActive = true;
    return true;
  default:
    break;
  }
  return false;
}

bool ANALYSER_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state))
    return true;

  if (key == KEY_0 && state == KEY_LONG_PRESSED) {
    ANALYSERMENU_Toggle();
    return true;
  }

  if (key == KEY_2 &&
      (state == KEY_LONG_PRESSED || state == KEY_LONG_PRESSED_CONT)) {
    if (!showSqTuner) {
      setRange(range.start,
               range.start + LCD_WIDTH * StepFrequencyTable[range.step]);
      return true;
    }
  }
  if (key == KEY_8 &&
      (state == KEY_LONG_PRESSED || state == KEY_LONG_PRESSED_CONT)) {
    if (!showSqTuner) {
      setRange(range.start,
               range.start + LCD_XCENTER * StepFrequencyTable[range.step]);
      return true;
    }
  }

  if (ANALYSERMENU_Key(key, state))
    return true;

  if (state == KEY_RELEASED) {
    if (key == KEY_EXIT) {
      if (listen) {
        listen = false;
        refreshSpectrumH();
        return true;
      }
      if (still) {
        exitStill();
        return true;
      }
      if (showSqTuner) {
        showSqTuner = false;
        return true;
      }
      if (showDbmTuner) {
        showDbmTuner = false;
        return true;
      }
    }
    if (key == KEY_F) {
      showSqTuner = !showSqTuner;
      if (showSqTuner) {
        showDbmTuner = false;
        // Грузим пресет только если уровень сменился с прошлого открытия
        if (ctx->squelch.value != sqEditLevel) {
          sqEditLevel = ctx->squelch.value;
          sqApplyThresholds(GetSqlPreset(sqEditLevel, ctx->frequency));
        }
        sqEditParam = SQ_EDIT_RSSI;
      }
      return true;
    }
    if (key == KEY_5) {
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
      FINPUT_Show(setRange);
      return true;
    }
  }

  // Long KEY_5: в STILL — переход на частоту, в scan-режимах — marker input
  if (key == KEY_5 && state == KEY_LONG_PRESSED) {
    if (still) {
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
      FINPUT_Show(onStillJumpFreq);
    } else {
      handleMarkerInput();
    }
    return true;
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED ||
      state == KEY_LONG_PRESSED_CONT) {
    if (showDbmTuner && dbmTunerKey(key, state))
      return true;
    if (showSqTuner && sqTunerKey(key, state))
      return true;
    if (still && stillModeKey(key, state))
      return true;
    if (!still)
      return analyzerModeKey(key, state);
  }
  return false;
}

// ── Init / deinit ──────────────────────────────────────────────────────────

static bool isValidRange(uint32_t start, uint32_t end) {
  return start < end && end <= BK4819_F_MAX &&
         (end - start) >= StepFrequencyTable[0];
}

void ANALYSER_init(void) {
  gMonitorMode = false;
  SPECTRUM_Y = 8;

  analyserSettingsLoad();
  ANALYSERMENU_SetDbmMin(aSettings.dbMin);
  ANALYSERMENU_SetDbmMax(aSettings.dbMax);

  if (aSettings.delay > 0 && aSettings.delay <= DELAY_MAX_US) {
    delay = aSettings.delay;
  } else {
    delay = DELAY_DEFAULT_US;
  }

  analyserMode =
      (aSettings.mode < MODE_COUNT) ? (AnalyserMode)aSettings.mode : MODE_SCAN;
  preciseMode = (aSettings.precise != 0);
  peaksFound = 0;

  range.step = RADIO_GetParam(ctx, PARAM_STEP);

  if (isValidRange(aSettings.lastRangeStart, aSettings.lastRangeEnd)) {
    range.start = aSettings.lastRangeStart;
    range.end = aSettings.lastRangeEnd;
  } else {
    range.start = DEFAULT_RANGE_START;
    range.end = range.start + StepFrequencyTable[range.step] * LCD_WIDTH;
  }

  msm = &vfo->msm;
  msm->f = range.start;
  scanF = range.start;
  targetF = range.start;

  markerA = (Marker){0};
  markerB = (Marker){0};

  showDbmTuner = false;
  showSqTuner = false;
  autoLevelMode = false;
  // preciseMode уже загружен из aSettings выше

  scanAwaitingSettle = false;
  settingsDirty = false;
  analyserSaveTime = 0;
  lastSquelchLevel = SQ_LEVEL_INVALID;
  lastSqMode = (AnalyserMode)0xFF;

  applySquelchPreset();

  SCAN_SetMode(SCAN_MODE_NONE);
  SPECTRUM_H = needSplit() ? SPECTRUM_H_SPLIT : SPECTRUM_H_FULL;
  SP_Init(&range);
  BANDS_RangePush(range);
}

void ANALYSER_deinit(void) {
  aSettings.dbMin = ANALYSERMENU_GetDbmMin();
  aSettings.dbMax = ANALYSERMENU_GetDbmMax();
  aSettings.delay = delay;
  aSettings.lastRangeStart = range.start;
  aSettings.lastRangeEnd = range.end;
  aSettings.mode = (uint8_t)analyserMode;
  aSettings.precise = preciseMode ? 1 : 0;
  analyserSettingsSave();
}

// ── Measurement / scan ─────────────────────────────────────────────────────

static bool sq_was_open; // предыдущее решение — для гистерезиса

static void measure(void) {
  msm->rssi = RADIO_GetRSSI(ctx);
  // msm->noise = RADIO_GetNoise(ctx);
  // msm->glitch = RADIO_GetGlitch(ctx);

  if (listen) {
    msm->open = true;
  } else if (still && sq_was_open) {
    msm->open = (msm->rssi > sq.rc) && (msm->noise < sq.nc) &&
                (glitchDisabled() || msm->glitch < sq.gc);
  } else {
    msm->open = (msm->rssi >= sq.ro) && (msm->noise < sq.no) &&
                (glitchDisabled() || msm->glitch < sq.go);
  }
  if (gMonitorMode)
    msm->open = true;

  sq_was_open = msm->open;
  LOOT_Update(msm);
  if (gSettings.skipGarbageFrequencies && (msm->f % 650000U == 0)) {
    msm->open = false;
  }
}

static void updateListening(void) {
  static uint32_t lastListenUpdate;
  if (Now() - lastListenUpdate >= SQL_DELAY) {
    if (preciseMode && !gMonitorMode) {
      // Без перетюнинга REG_30 не сбрасывается — делаем pulse вручную
      uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
      BK4819_WriteRegister(BK4819_REG_30, 0x200);
      BK4819_WriteRegister(BK4819_REG_30, reg);
      SYSTICK_DelayUs(delay);
    }
    measure();
    lastListenUpdate = Now();
  }
}

static void updateScan(void) {
  if (delay <= DELAY_BLOCKING_MAX_US) {
    RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, preciseMode, false);
    RADIO_SetParam(ctx, PARAM_FREQUENCY, scanF, false);
    RADIO_ApplySettings(ctx);
    HRTIME_DelayUs(delay);
  } else {
    if (!scanAwaitingSettle) {
      RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, preciseMode, false);
      RADIO_SetParam(ctx, PARAM_FREQUENCY, scanF, false);
      RADIO_ApplySettings(ctx);
      scanWaitUntil = Now() + (delay / 1000);
      scanAwaitingSettle = true;
      return;
    }
    if (Now() < scanWaitUntil)
      return;
    scanAwaitingSettle = false;
  }

  // Фиксируем частоту измерения ДО measure(), чтобы LOOT_Update попал в неё
  msm->f = scanF;
  measure();
  if (!still) {
    SP_AddPoint(msm);
  }

  if (still)
    return;

  // Двигаем только курсор; msm->f остаётся равной частоте последнего измерения
  scanF += StepFrequencyTable[range.step];

  if (scanF > range.end) {
    if (analyserMode == MODE_PEAKS) {
      updatePeaks();
      updateMarkersFromPeaks();
    }
    scanF = range.start;
    gRedrawScreen = true;
    SP_Begin();
  }
}

void ANALYSER_update(void) {
  applySquelchPreset();
  ANALYSER_UpdateSave();

  // Deferred single-tap KEY_6 → lockToPeak, если второй тап не пришёл
  if (key6PendingSingle && (Now() - key6PendingSingle) >= KEY6_DOUBLE_TAP_MS) {
    key6PendingSingle = 0;
    lockToPeak();
  }

  bool shouldListen = still || (analyserMode == MODE_SCAN);

  if (shouldListen) {
    if (vfo->is_open) {
      updateListening();
    } else {
      updateScan();
    }
    if (vfo->is_open != msm->open) {
      vfo->is_open = msm->open;
      gRedrawScreen = true;
      if (msm->open) {
        targetF = msm->f;
      }
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
    }
  } else {
    // В SCAN и PEAKS не слушаем — гарантируем выключение аудио
    if (vfo->is_open) {
      vfo->is_open = false;
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
    }
    updateScan();
  }
}

// ── Render ─────────────────────────────────────────────────────────────────

// Частота в единицах 10Гц → МГц с 3 знаками после точки (kHz-точность)
static void printFreqKHz(uint8_t x, uint8_t y, uint8_t pos, uint32_t f) {
  PrintSmallEx(x, y, pos, C_FILL, "%u.%03u", f / 100000u, (f / 100u) % 1000u);
}

static void renderBottomFreq(void) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  bool showCur = (Now() < cursorRangeTimeout);
  Band r = CUR_GetRange(&range, step);

  uint32_t leftF = showCur ? r.start : range.start;
  uint32_t rightF = showCur ? r.end : range.end;

  printFreqKHz(1, LCD_HEIGHT - 2, POS_L, leftF);
  printFreqKHz(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, rightF);

  if (markerA.f && markerB.f) {
    // ΔF между маркерами
    uint32_t dF = (markerB.f > markerA.f) ? (markerB.f - markerA.f)
                                          : (markerA.f - markerB.f);
    char buf[16];
    mhzToS(buf, dF);
    PrintSmallEx(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, C_FILL, "d=%s", buf);
  } else {
    // Центр-частота диапазона (деление до суммы — защита от переполнения)
    uint32_t cF = leftF / 2 + rightF / 2;
    printFreqKHz(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, cF);
  }
}

static void renderSqTuner(void) {
  const char *labels[] = {"R", "N", "G"};
  uint16_t values[] = {sq.ro, sq.no, sq.go};
  uint8_t rows = glitchDisabled() ? 2 : 3;

  for (uint8_t i = 0; i < rows; i++) {
    uint8_t y = 18 + 6 * i;
    if (i == sqEditParam) {
      FillRect(LCD_WIDTH - 30, y - 5, 30, 7, C_FILL);
      PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_CLEAR, "%s %3u", labels[i],
                   values[i]);
    } else {
      PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_FILL, "%s %3u", labels[i],
                   values[i]);
    }
  }
}

// Оверлей dBm tuner: значения max/min + строка подсказки клавиш снизу.
// Очищаем фон и обводим рамкой для читаемости поверх любых баров.
static void renderDbmTuner(void) {
  int16_t dbMin = ANALYSERMENU_GetDbmMin();
  int16_t dbMax = ANALYSERMENU_GetDbmMax();

  const uint8_t bw = 62;
  const uint8_t bh = 21;
  const uint8_t bx = LCD_XCENTER - bw / 2;
  const uint8_t by = 10;

  FillRect(bx, by, bw, bh, C_CLEAR);
  DrawRect(bx, by, bw, bh, C_FILL);

  PrintSmallEx(LCD_XCENTER, by + 5, POS_C, C_FILL, "max %4d", dbMax);
  PrintSmallEx(LCD_XCENTER, by + 11, POS_C, C_FILL, "min %4d", dbMin);
  PrintSmallEx(LCD_XCENTER, by + 18, POS_C, C_FILL, "2/8 min 3/9 max");
}

// Таблица маркеров под спектром (режим PEAKS): 2 строки по 6 пикс.
// Manual-маркер метки выделяется инверсией, auto — обычная буква.
static void renderMarkersTable(void) {
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H_SPLIT + SPLIT_GAP;
  const Marker *markers[2] = {&markerA, &markerB};
  const char labels[2] = {'A', 'B'};

  for (uint8_t i = 0; i < 2; i++) {
    const Marker *m = markers[i];
    if (!m->f)
      continue; // маркер не установлен

    uint8_t xp = SP_F2X(m->f);
    uint16_t rssi = (xp != 0xFF) ? SP_GetPointRSSI(xp) : 0;
    uint16_t n = (xp != 0xFF) ? SP_GetPointNoise(xp) : 0;
    uint16_t g = (xp != 0xFF) ? SP_GetPointGlitch(xp) : 0;

    uint8_t y = y0 + 5 + i * 6;

    if (m->manual) {
      FillRect(0, y - 5, 6, 6, C_FILL);
      PrintSmallEx(1, y, POS_L, C_CLEAR, "%c", labels[i]);
    } else {
      PrintSmallEx(1, y, POS_L, C_FILL, "%c", labels[i]);
    }
    if (glitchDisabled()) {
      PrintSmallEx(7, y, POS_L, C_FILL, "%u.%03u %3u %3u", m->f / 100000u,
                   m->f / 100u % 1000u, rssi, n);
    } else {
      PrintSmallEx(7, y, POS_L, C_FILL, "%u.%03u %3u %3u %3u", m->f / 100000u,
                   m->f / 100u % 1000u, rssi, n, g);
    }
  }
}

// SCAN: частота последнего активного лута сверху + RSSI bar при прослушке
static void renderScanListen(void) {
  if (gLastActiveLoot) {
    PrintMediumEx(LCD_XCENTER, 14, POS_C, C_FILL, "%u.%03u",
                  gLastActiveLoot->f / 100000u,
                  (gLastActiveLoot->f / 100u) % 1000u);
  }
  if (vfo->is_open) {
    UI_RSSIBar(16);
  }
}

// RSSI bar в area под спектром (STILL/listen)
static void renderStillRssiBar(void) {
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H_SPLIT + SPLIT_GAP;
  UI_RSSIBar(y0 + 6);
}

static void renderStillInfo(void) {
  SP_RenderArrow(RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintMediumEx(LCD_XCENTER, 14, POS_C, C_FILL,
                RADIO_GetParamValueString(ctx, PARAM_FREQUENCY));

  if (glitchDisabled()) {
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "R%u N%u", msm->rssi,
                 msm->noise);
  } else {
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "R%u N%u G%u",
                 msm->rssi, msm->noise, msm->glitch);
  }

  if (gMonitorMode)
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "MONITOR");
}

// Стрелка + буква метки возле неё. Manual выделяется инверсией.
static void renderUserMarker(const Marker *m, char label) {
  if (!m->f)
    return;
  uint8_t x = SP_F2X(m->f);
  if (x == 0xFF)
    return;
  SP_RenderArrow(m->f);

  uint8_t ly = SPECTRUM_Y + SPECTRUM_H - 1;
  uint8_t lx = (x + 6 < LCD_WIDTH) ? x + 2 : x - 5;

  if (m->manual) {
    FillRect(lx - 1, ly - 5, 5, 6, C_FILL);
    PrintSmallEx(lx, ly, POS_L, C_CLEAR, "%c", label);
  } else {
    FillRect(lx - 1, ly - 5, 5, 6, C_CLEAR);
    PrintSmallEx(lx, ly, POS_L, C_FILL, "%c", label);
  }
}

void ANALYSER_render(void) {
  STATUSLINE_RenderRadioSettings();

  VMinMax v = autoLevelMode
                  ? SP_GetAutoLevel()
                  : (VMinMax){.vMin = DBm2Rssi(ANALYSERMENU_GetDbmMin()),
                              .vMax = DBm2Rssi(ANALYSERMENU_GetDbmMax())};
  SP_Render(&range, v);
  renderBottomFreq();

  // Левый верх: базовый режим → (опц. оверлей инверсно) → delay.
  // Оверлей вынесен отдельно, чтобы режим всегда был виден.
  uint8_t sx = 0;
  PrintSmallEx(sx, 12, POS_L, C_FILL, "%s", getModeStr());
  sx += 14;
  char ov = getOverlayChar();
  if (ov) {
    FillRect(sx - 1, 7, 6, 6, C_FILL);
    PrintSmallEx(sx, 12, POS_L, C_CLEAR, "%c", ov);
    sx += 7;
  }
  PrintSmallEx(sx, 12, POS_L, C_FILL, "%uus", delay);
  PrintSmallEx(LCD_WIDTH - 1, 12, POS_R, C_FILL, "%s",
               RADIO_GetParamValueString(ctx, PARAM_STEP));

  if (still || listen) {
    renderStillInfo();
    renderStillRssiBar();
  } else {
    // scan-режимы: SCAN — loot freq + RSSI bar; PEAKS — таблица маркеров
    switch (analyserMode) {
    case MODE_PEAKS:
      renderMarkersTable();
      break;
    default:
      renderScanListen();
      break;
    }
    if (Now() < cursorRangeTimeout)
      CUR_Render();
    if (showDbmTuner) {
      renderDbmTuner();
    }
  }

  // Пользовательские маркеры видны в scan-режимах
  if (!still && !showSqTuner) {
    renderUserMarker(&markerA, 'A');
    renderUserMarker(&markerB, 'B');
  }

  if (showSqTuner) {
    renderSqTuner();
    uint16_t threshold = 0;
    switch (sqEditParam) {
    case SQ_EDIT_RSSI:
      threshold = sq.ro;
      break;
    case SQ_EDIT_NOISE:
      threshold = sq.no;
      break;
    case SQ_EDIT_GLITCH:
      threshold = sq.go;
      break;
    }
    SP_RenderLine(threshold, v);
  }

  REGSMENU_Draw();
  ANALYSERMENU_Draw();
}
