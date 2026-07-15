#include "scaner.h"
#include "../dcs.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/lootlist.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdint.h>

static bool pttWasLongPressed = false;

static char String[16];

static bool showSpectrum;

// Для определения состояния CHK: отслеживаем изменение частоты
static uint32_t lastTrackedF = 0;
static uint32_t lastFChangeMs = 0;
// Порог: если частота не менялась дольше этого — скорее всего режим проверки
// Задержка сканирования по умолчанию 1200 мкс = 1.2 мс, так что
// 30 мс стабильности гарантированно означают останов
#define CHECK_STABLE_MS 30

static void setRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
  BANDS_RangeClear();
  SCAN_SetRange(fs, fe);
  BANDS_RangePush(gCurrentBand);
}

static void initBand(void) {
  if (gCurrentBand.detached) {
    LogC(LOG_C_BRIGHT_YELLOW, "[i] [SCAN] Init withOUT detached band");
    gCurrentBand = BANDS_ByFrequency(RADIO_GetParam(ctx, PARAM_FREQUENCY));
    gCurrentBand.detached = true;
  } else {
    LogC(LOG_C_BRIGHT_YELLOW, "[i] [SCAN] Init with detached band");
    if (!gCurrentBand.start && !gCurrentBand.end) {
      gCurrentBand = DEFAULT_BAND;
    }
  }
  if (gCurrentBand.start == DEFAULT_BAND.start &&
      gCurrentBand.end == DEFAULT_BAND.end) {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    gCurrentBand.start = ctx->frequency - 64 * step;
    gCurrentBand.end = gCurrentBand.start + 128 * step;
  }
}

void SCANER_init(void) {
  SPECTRUM_Y = 16;
  SPECTRUM_H = LCD_HEIGHT - SPECTRUM_Y - 16 - 7;

  gMonitorMode = false;
  lastTrackedF = 0;
  lastFChangeMs = 0;

  initBand();

  gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
  BANDS_RangeClear();
  BANDS_RangePush(gCurrentBand);

  SCAN_SetDelay(1800);

  SCAN_SetMode(SCAN_MODE_FREQUENCY);
  SCAN_Init();
}

ScanState oldScanState;
void SCANER_update(void) {
  ScanState state = SCAN_GetState();
  if (state != oldScanState) {
    oldScanState = state;
    gRedrawScreen = true;
  }
}

// Сдвиг диапазона на одну ширину вверх или вниз
static void shiftBand(bool up) {
  uint32_t width = gCurrentBand.end - gCurrentBand.start;
  Band *b = BANDS_RangePeek();
  if (up) {
    b->start += width;
    b->end += width;
  } else {
    if (b->start > width) {
      b->start -= width;
      b->end -= width;
    }
  }
  gCurrentBand = *b;
  SCAN_SetBand(gCurrentBand);
}

static bool handleLongPress(KEY_Code_t key) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  Band _b;

  switch (key) {
  case KEY_6:
    if (!gLastActiveLoot)
      return false;
    _b = gCurrentBand;
    _b.start = gLastActiveLoot->f - step * 64;
    _b.end = _b.start + step * 128;
    BANDS_RangePush(_b);
    SCAN_SetBand(*BANDS_RangePeek());
    return true;

  case KEY_PTT:
    if (gSettings.keylock) {
      pttWasLongPressed = true;
      LOOT_WhitelistLast();
      SCAN_Next();
      return true;
    }
    return false;

  default:
    return false;
  }
}

static bool handleRepeatableKeys(KEY_Code_t key) {
  switch (key) {
  case KEY_1:
  case KEY_7:
    SCAN_SetDelay(
        AdjustU(SCAN_GetDelay(), 0, 10000, key == KEY_1 ? 100 : -100));
    return true;

  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
    SCAN_SetBand(gCurrentBand);
    return true;

  case KEY_UP:
  case KEY_DOWN:
    shiftBand((key == KEY_UP) ^ gSettings.invertButtons);
    return true;

  case KEY_6:
    showSpectrum = !showSpectrum;
    return true;

  default:
    return false;
  }
}

static bool handlePTTRelease(void) {
  if (gLastActiveLoot && !gSettings.keylock) {
    uint32_t targetF = gLastActiveLoot->f;
    APPS_run(APP_VFO1);
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, true);
    RADIO_ApplySettings(ctx);
    RADIO_SaveCurrentVFO(gRadioState);
    return true;
  }

  if (gSettings.keylock && !pttWasLongPressed) {
    pttWasLongPressed = false;
    SCAN_NextBlacklist();
    return true;
  }

  return false;
}

static bool handleRelease(KEY_Code_t key) {
  switch (key) {

  case KEY_5:
    gFInputCallback = setRange;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
    gFInputValue1 = 0;
    FINPUT_init();
    gFInputActive = true;
    return true;

  case KEY_SIDE1:
    SCAN_NextBlacklist();
    return true;

  case KEY_SIDE2:
    SCAN_NextWhitelist();
    return true;

  case KEY_STAR:
    LOOTLIST_init();
    gLootlistActive = true;
    return true;

  case KEY_2: {
    // Zoom in: сужаем диапазон вокруг текущей частоты
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    uint32_t center = RADIO_GetParam(ctx, PARAM_FREQUENCY);
    Band _b = gCurrentBand;
    _b.start = center - step * 32;
    _b.end = center + step * 32;
    BANDS_RangePush(_b);
    gCurrentBand = *BANDS_RangePeek();
    SCAN_SetBand(gCurrentBand);
    return true;
  }

  case KEY_8:
    // Zoom out: назад к предыдущему диапазону
    BANDS_RangePop();
    gCurrentBand = *BANDS_RangePeek();
    SCAN_SetBand(gCurrentBand);
    return true;

  case KEY_PTT:
    return handlePTTRelease();

  default:
    return false;
  }
}

bool SCANER_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state)) {
    return true;
  }

  if (state == KEY_PRESSED && key == KEY_PTT) {
    pttWasLongPressed = false;
  }

  if (state == KEY_LONG_PRESSED) {
    return handleLongPress(key);
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (handleRepeatableKeys(key)) {
      return true;
    }
  }

  if (state == KEY_RELEASED) {
    return handleRelease(key);
  }

  return false;
}

// ─── Рендер ───────────────────────────────────────────────────────────────

// Горизонтальный "прогресс" сканирования: позиция внутри диапазона
static void renderScanProgress(uint8_t y, uint32_t f, ScanState state) {
  uint8_t px = SP_F2X(f);
  px = ConvertDomain(px, 0, LCD_WIDTH, 24, LCD_WIDTH);

  DrawRect(24, y - 3, LCD_WIDTH - 24, 5, C_FILL);

  switch (state) {
  case SCAN_STATE_IDLE:
    DrawVLine(px, y - 2, 3, C_FILL);
    break;
  case SCAN_STATE_TUNING:
    DrawVLine(px, y - 2, 3, C_FILL);
    PutPixel(px + 1, y - 1, C_FILL);
    break;
  case SCAN_STATE_CHECKING:
    FillRect(px, y - 2, 3, 3, C_FILL);
    break;
  case SCAN_STATE_LISTENING:
    FillRect(px, y - 2, 3, 3, C_FILL);
    break;
  }
  PrintSmallEx(0, y + 1, POS_L, C_FILL, "%u/s", SCAN_GetCps());
}

static void renderBandBounds(uint8_t y) {
  FSmall(0, y, POS_L, gCurrentBand.start);
  FSmall(LCD_WIDTH, y, POS_R, gCurrentBand.end);
  if (BANDS_RangeIndex() > 0) {
    PrintSmallEx(LCD_XCENTER, y, POS_C, C_FILL, "Z%u", BANDS_RangeIndex() + 1);
  }
}
static void renderLootInfo(uint8_t y) {
  if (!gLastActiveLoot)
    return;

  UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, y, POS_C);

  const uint32_t ago = (Now() - gLastActiveLoot->lastTimeOpen) / 1000;
  if (ago) {
    PrintSmallEx(LCD_WIDTH, y, POS_R, C_FILL, "%u:%02u", ago / 60, ago % 60);
  }
}

void SCANER_render(void) {
  const uint8_t BASE = 40;
  const uint32_t f = RADIO_GetParam(ctx, PARAM_FREQUENCY);
  const uint32_t step = StepFrequencyTable[ctx->step];

  STATUSLINE_RenderRadioSettings();

  // Строка 1 (y=14): задержка слева, имя диапазона по центру, шаг справа
  PrintSmallEx(0, 12, POS_L, C_FILL, "%uus", SCAN_GetDelay());
  PrintSmallEx(LCD_WIDTH, 12, POS_R, C_FILL, "%d.%02d", step / KHZ, step % KHZ);

  ScanState state = SCAN_GetState();

  if (showSpectrum) {
    SP_Render(&gCurrentBand, SP_GetMinMax());
  } else {
    uint8_t y = 15 + 7;
    uint8_t cnt = 0;

    for (int16_t i = LOOT_Size() - 1; i >= 0 && cnt < 4; --i) {
      Loot *v = LOOT_Item(i);
      const uint32_t ago = (Now() - v->lastTimeOpen) / 1000;
      mhzToS(String, v->f);

      PrintMediumEx(0, y, POS_L, C_FILL, "%s %02u:%02u", String, ago / 60,
                    ago % 60);

      if (v->code != 255) {
        if (v->isCd) {
          PrintRTXCode(String, CODE_TYPE_DIGITAL, v->code);
        } else {
          PrintRTXCode(String, CODE_TYPE_CONTINUOUS_TONE, v->code);
        }
        PrintMediumEx(LCD_WIDTH - 1, y, POS_R, C_FILL, "%s", String);
      }
      cnt++;
      y += 8;
    }
  }

  if (state == SCAN_STATE_LISTENING) {
    UI_RSSIBar(14 + 1);
  }

  if (gLastActiveLoot) {
    UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 14, POS_C);
  }

  renderScanProgress(LCD_HEIGHT - 6 - 4, f, state);
  renderBandBounds(LCD_HEIGHT - 2);
  PrintSmallEx(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, C_FILL, "%s",
               RADIO_GetParamValueString(ctx, PARAM_FREQUENCY));

  REGSMENU_Draw();
}

void SCANER_deinit(void) {}
