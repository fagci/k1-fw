#include "lootlist.h"
#include "../apps/apps.h"
#include "../dcs.h"
#include "../driver/bk4829.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/menu.h"
#include "../helper/scan.h"
#include "../helper/storage.h"
#include "../inc/band.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include <stdbool.h>
#include <stdint.h>

bool gLootlistActive;
static ScanMode prevScanMode = SCAN_MODE_NONE;
static uint8_t menuIndex = 0;
static const uint8_t MENU_ITEM_H_LARGER = 15;
static void initMenu();

// Edit mode state
typedef enum {
  EDIT_MODE_NONE,
  EDIT_MODE_ACTIVE,
  EDIT_MODE_FINPUT,
} EditMode;

static EditMode gEditMode = EDIT_MODE_NONE;
static uint16_t gEditLootIndex = 0;
static uint8_t gEditField = 0;

// Fields: 0=Freq, 1=Modulation, 2=BW, 3=Radio, 4=Squelch, 5=Gain, 6=Code
#define EDIT_FIELD_COUNT 7
static const char *EDIT_FIELD_NAMES[] = {
    "Frequency", "Modulation", "Bandwidth", "Radio",
    "Squelch",   "Gain",       "CTCSS/DCS",
};

typedef enum {
  SORT_LOT,
  SORT_DUR,
  SORT_BL,
  SORT_F,
} Sort;

bool (*sortings[])(const Loot *a, const Loot *b) = {
    LOOT_SortByLastOpenTime,
    LOOT_SortByDuration,
    LOOT_SortByBlacklist,
    LOOT_SortByF,
};

static char *sortNames[] = {
    "last open",
    "duration",
    "blacklist",
    "freq",
};

static Sort sortType = SORT_LOT;

static bool shortList = true;
static bool sortRev = false;

static void tuneToLoot(const Loot *loot, bool save) {
  RADIO_SetParam(ctx, PARAM_BANDWIDTH, loot->bw, true);
  RADIO_SetParam(ctx, PARAM_GAIN, loot->gainIndex, true);
  RADIO_SetParam(ctx, PARAM_MODULATION, loot->modulation, true);
  RADIO_SetParam(ctx, PARAM_SQUELCH_TYPE, loot->squelch_type, true);
  RADIO_SetParam(ctx, PARAM_SQUELCH_VALUE, loot->squelch_value, true);
  RADIO_SetParam(ctx, PARAM_RADIO, loot->radio, true);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, loot->f, save);
  RADIO_ApplySettings(ctx);
}

static void displayFreqBlWl(uint8_t y, const Loot *loot) {
  UI_DrawLoot(loot, 1, y + 7, POS_L);
}

static void getLootItem(uint16_t i, uint16_t index) {
  const Loot *item = LOOT_Item(index);
  const uint8_t y = MENU_Y + i * MENU_ITEM_H_LARGER;

  displayFreqBlWl(y, item);

  PrintSmallEx(LCD_WIDTH - 6, y + 7, POS_R, C_INVERT, "%us", item->duration);

  if (item->code != 0xFF) {
    if (item->isCd) {
      PrintSmallEx(8 + 55, y + 7 + 6, POS_L, C_INVERT, "DCS:D%03oN",
                   DCS_Options[item->code]);
    } else {
      PrintSmallEx(8 + 55, y + 7 + 6, POS_L, C_INVERT, "CT:%u.%uHz",
                   CTCSS_Options[item->code] / 10,
                   CTCSS_Options[item->code] % 10);
    }
  }
}

static void getLootItemShort(uint16_t i, uint16_t index) {
  const Loot *loot = LOOT_Item(index);
  const uint8_t x = LCD_WIDTH - 6;
  const uint8_t y = MENU_Y + i * MENU_ITEM_H;
  // wrap-арифметика uint16_t: корректно для разниц до 65535 сек ≈ 18ч
  const uint16_t now_s = (uint16_t)(Now() / 1000);
  const uint32_t ago = (uint16_t)(now_s - loot->lastTimeOpen);

  displayFreqBlWl(y, loot);

  switch (sortType) {
  case SORT_LOT:
    PrintSmallEx(x, y + 7, POS_R, C_INVERT, "%u:%02u", ago / 60, ago % 60);
    break;
  case SORT_DUR:
  case SORT_BL:
  case SORT_F:
    PrintSmallEx(x, y + 7, POS_R, C_INVERT, "%us", loot->duration);
    break;
  }
}

static void renderItem(uint16_t index, uint8_t i) {
  (shortList ? getLootItemShort : getLootItem)(i, index);
}

static void sort(Sort type) {
  if (sortType == type) {
    sortRev = !sortRev;
  } else {
    sortRev = type == SORT_DUR;
  }
  LOOT_Sort(sortings[type], sortRev);
  sortType = type;
  STATUSLINE_SetText("By %s %s", sortNames[sortType], sortRev ? "v" : "^");
}

static void saveLootToCh(const Loot *loot, int16_t chnum, uint16_t scanlist) {
  CH ch = LOOT_ToCh(loot);
  ch.scanlists = scanlist;
  STORAGE_SAVE("Channels.ch", chnum, &ch);
  STATUSLINE_SetText("Saved to CH %d", chnum);
}

static void showSaveProgress(uint32_t saved, uint32_t total) {
  FillRect(0, LCD_YCENTER - 4, LCD_WIDTH, 9, C_FILL);
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER + 3, POS_C, C_INVERT,
                "Saving... %lu/%lu", saved, total);
  ST7565_Blit();
}

static void cbSaveToChannel(uint32_t chnum, uint32_t _) {
  (void)_;
  // Save the currently selected loot item (from menu index)
  const Loot *loot = LOOT_Item(menuIndex);
  saveLootToCh(loot, (int16_t)chnum, gSettings.currentScanlist);
  gFInputActive = false;
  gRedrawScreen = true;
}

static void saveToFreeChannels(bool saveWhitelist, uint16_t scanlist) {
  FillRect(0, LCD_YCENTER - 4, LCD_WIDTH, 9, C_FILL);
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER + 3, POS_C, C_INVERT, "Saving...");
  ST7565_Blit();
  uint32_t saved = 0;
  uint16_t chnum = 4096; // Move outside loop to prevent saving to same channel

  for (uint16_t i = 0; i < LOOT_Size() && chnum > 0; ++i) {
    const Loot *loot = LOOT_Item(i);
    if (saveWhitelist && !loot->whitelist) {
      continue;
    }
    if (!saveWhitelist && !loot->blacklist) {
      continue;
    }

    CH ch;
    while (chnum > 0) {
      chnum--;
      STORAGE_LOAD("Channels.ch", chnum, &ch);
      if (!IsReadable(ch.name)) {
        // save new
        saveLootToCh(loot, chnum, scanlist);
        saved++;
        break;
      } else {
        if (ch.rxF == loot->f) {
          break;
        }
      }
    }
  }

  FillRect(0, LCD_YCENTER - 4, LCD_WIDTH, 9, C_FILL);
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER + 3, POS_C, C_INVERT, "Saved: %u",
                    saved);
  ST7565_Blit();
  SYSTICK_DelayMs(2000);
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {

  Loot *loot;
  loot = LOOT_Item(index);
  const uint8_t MENU_SIZE = LOOT_Size();

  VFOContext *ctx = &RADIO_GetCurrentVFO(gRadioState)->context;

  if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_0:
      LOOT_Clear();
      RADIO_SetParam(ctx, PARAM_FREQUENCY, 0, false);
      RADIO_ApplySettings(ctx);
      initMenu();
      return true;
    case KEY_SIDE1:
      gMonitorMode = !gMonitorMode;
      return true;
    case KEY_8:
      saveToFreeChannels(false, 1 << 15);
      return true;
    case KEY_5:
      saveToFreeChannels(true, gSettings.currentScanlist);
      return true;
    case KEY_F:
      // Save loot list to file
      if (LOOT_Save()) {
        STATUSLINE_SetText("Loot saved");
      } else {
        STATUSLINE_SetText("Save failed!");
      }
      return true;
    /* case KEY_STAR:
      // TODO: select any of SL
      CHANNELS_LoadBlacklistToLoot();
      initMenu();
      return true; */
    default:
      break;
    }
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    switch (key) {
    case KEY_UP:
    case KEY_DOWN:
      tuneToLoot(loot, false);
      return true;
    default:
      break;
    }
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      MENU_Deinit();
      gLootlistActive = false;
      SCAN_SetMode(prevScanMode);
      return true;
    case KEY_PTT:
      tuneToLoot(loot, true);
      APPS_run(APP_VFO1);
      return true;
    case KEY_1:
      sort(SORT_LOT);
      return true;
    case KEY_2:
      sort(SORT_DUR);
      return true;
    case KEY_3:
      sort(SORT_BL);
      return true;
    case KEY_4:
      sort(SORT_F);
      return true;
    case KEY_SIDE1:
      loot->whitelist = false;
      loot->blacklist = !loot->blacklist;
      return true;
    case KEY_SIDE2:
      loot->blacklist = false;
      loot->whitelist = !loot->whitelist;
      return true;
    case KEY_7:
      shortList = !shortList;
      initMenu();
      return true;
    case KEY_9:
      // Load loot list from file
      if (LOOT_Load()) {
        STATUSLINE_SetText("Loot loaded");
        initMenu();
        if (LOOT_Size()) {
          tuneToLoot(LOOT_Item(0), false);
        }
      } else {
        STATUSLINE_SetText("Load failed!");
      }
      return true;
    /* case KEY_5:
      tuneToLoot(loot, false);
      gChListFilter = TYPE_FILTER_CH_SAVE;
      APPS_run(APP_CH_LIST);
      return true; */
    case KEY_0:
      LOOT_Remove(menuIndex);
      if (menuIndex > LOOT_Size() - 1) {
        menuIndex = LOOT_Size() - 1;
      }
      loot = LOOT_Item(menuIndex);
      if (loot) {
        tuneToLoot(loot, false);
      } else {
        RADIO_SetParam(ctx, PARAM_FREQUENCY, 0, false);
        RADIO_ApplySettings(ctx);
      }
      return true;
    case KEY_MENU:
      tuneToLoot(loot, true);
      APPS_exit();
      return true;
    case KEY_6:
      // Enter edit mode for current item
      LOOTLIST_StartEdit(menuIndex);
      return true;
    case KEY_5:
      // Save to specific channel number
      if (LOOT_Size() > 0) {
        UI_ClearScreen();
        PrintMediumEx(LCD_XCENTER, 30, POS_C, C_FILL, "Save to CH#");
        PrintSmallEx(LCD_XCENTER, 45, POS_C, C_FILL,
                     "Enter number, EXIT:cancel");
        ST7565_Blit();

        gFInputCallback = cbSaveToChannel;
        FINPUT_setup(0, 4095, UNIT_RAW, false);
        gFInputValue1 = 0;
        gFInputValue2 = 0;
        FINPUT_init();
        gFInputActive = true;
      }
      return true;
    default:
      break;
    }
  }
  return false;
}

static void cbSetLootFreq(uint32_t f, uint32_t _) {
  (void)_;
  Loot *loot = LOOT_Item(gEditLootIndex);
  loot->f = f;
  gEditMode = EDIT_MODE_ACTIVE;
  gFInputActive = false;
  gRedrawScreen = true;
  STATUSLINE_SetText("Freq: %lu.%05lu", f / MHZ, f % MHZ);
}

static void editLootField(uint16_t index, uint8_t field) {
  Loot *loot = LOOT_Item(index);

  switch (field) {
  case 0: // Frequency
    gFInputCallback = cbSetLootFreq;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    gFInputValue1 = loot->f;
    gFInputValue2 = 0;
    FINPUT_init();
    gEditMode = EDIT_MODE_FINPUT;
    break;
  case 1:                                          // Modulation
    loot->modulation = (loot->modulation + 1) % 5; // FM, AM, WFM, USB, LSB
    STATUSLINE_SetText("Mod: %s", loot->modulation == 0   ? "FM"
                                  : loot->modulation == 1 ? "AM"
                                  : loot->modulation == 2 ? "WFM"
                                  : loot->modulation == 3 ? "USB"
                                                          : "LSB");
    break;
  case 2: // Bandwidth
    loot->bw = (loot->bw + 1) % 6;
    STATUSLINE_SetText("BW: %d", loot->bw);
    break;
  case 3:                                // Radio
    loot->radio = (loot->radio + 1) % 3; // BK4819, BK1080, SI4732
    STATUSLINE_SetText("Radio: %s", loot->radio == 0   ? "BK4819"
                                    : loot->radio == 1 ? "BK1080"
                                                       : "SI4732");
    break;
  case 4:                                              // Squelch type
    loot->squelch_type = (loot->squelch_type + 1) % 4; // OFF, NOI, N+M, M
    STATUSLINE_SetText("SQL: %s", loot->squelch_type == 0   ? "OFF"
                                  : loot->squelch_type == 1 ? "NOI"
                                  : loot->squelch_type == 2 ? "N+M"
                                                            : "M");
    break;
  case 5: // Gain
    loot->gainIndex = (loot->gainIndex + 1) % 32;
    STATUSLINE_SetText("Gain: %d", loot->gainIndex);
    break;
  case 6: // CTCSS/DCS code
    loot->code = (loot->code + 1) % 256;
    if (loot->code == 255) {
      STATUSLINE_SetText("Code: OFF");
    } else {
      STATUSLINE_SetText("Code: %d", loot->code);
    }
    break;
  }
}

void LOOTLIST_StartEdit(uint16_t index) {
  gEditLootIndex = index;
  gEditField = 0;
  gEditMode = EDIT_MODE_ACTIVE;
}

static void renderEditMode(void) {
  if (gEditMode == EDIT_MODE_FINPUT) {
    UI_ClearScreen();
    FINPUT_render();
    return;
  }

  // Clear screen before rendering edit mode
  UI_ClearScreen();

  Loot *loot = LOOT_Item(gEditLootIndex);
  uint8_t sel = gEditField;

  // Header
  PrintMediumEx(LCD_XCENTER, 8, POS_C, C_FILL, "Edit Loot #%u", gEditLootIndex);
  DrawLine(0, 10, LCD_WIDTH - 1, 10, C_FILL);

// Highlight selected row
#define FIELD_Y(f) (17 + (f) * 7)
  FillRect(0, FIELD_Y(sel) - 5, LCD_WIDTH, 7, C_FILL);

  // Draw fields
  for (uint8_t i = 0; i < EDIT_FIELD_COUNT; i++) {
    uint8_t y = FIELD_Y(i);
    uint8_t color = (i == sel) ? C_INVERT : C_FILL;

    switch (i) {
    case 0: // Frequency
      PrintSmallEx(3, y, POS_L, color, "Freq: %lu.%05lu", loot->f / MHZ,
                   loot->f % MHZ);
      break;
    case 1: // Modulation
      PrintSmallEx(3, y, POS_L, color, "Mod: %s",
                   loot->modulation == 0   ? "FM"
                   : loot->modulation == 1 ? "AM"
                   : loot->modulation == 2 ? "WFM"
                   : loot->modulation == 3 ? "USB"
                                           : "LSB");
      break;
    case 2: // Bandwidth
      PrintSmallEx(3, y, POS_L, color, "BW: %d", loot->bw);
      break;
    case 3: // Radio
      PrintSmallEx(3, y, POS_L, color, "Radio: %s",
                   loot->radio == 0   ? "BK4819"
                   : loot->radio == 1 ? "BK1080"
                                      : "SI4732");
      break;
    case 4: // Squelch
      PrintSmallEx(3, y, POS_L, color, "SQL: %s:%d",
                   loot->squelch_type == 0   ? "OFF"
                   : loot->squelch_type == 1 ? "NOI"
                   : loot->squelch_type == 2 ? "N+M"
                                             : "M",
                   loot->squelch_value);
      break;
    case 5: // Gain
      PrintSmallEx(3, y, POS_L, color, "Gain: %d", loot->gainIndex);
      break;
    case 6: // Code
      if (loot->code == 255) {
        PrintSmallEx(3, y, POS_L, color, "Code: OFF");
      } else if (loot->isCd) {
        PrintSmallEx(3, y, POS_L, color, "DCS: D%03oN",
                     DCS_Options[loot->code]);
      } else {
        PrintSmallEx(3, y, POS_L, color, "CT: %u.%uHz",
                     CTCSS_Options[loot->code] / 10,
                     CTCSS_Options[loot->code] % 10);
      }
      break;
    }
  }

  // Footer hint
  PrintSmallEx(LCD_XCENTER, 63, POS_C, C_FILL, "UP/DN:Field MENU:Edit");
}

static bool editModeKey(KEY_Code_t key, Key_State_t state) {
  if (gEditMode == EDIT_MODE_FINPUT) {
    // Allow EXIT to cancel FINPUT and return to edit mode
    if (key == KEY_EXIT && state == KEY_RELEASED) {
      gFInputActive = false;
      gEditMode = EDIT_MODE_ACTIVE;
      gRedrawScreen = true;
      return true;
    }
    return false; // Let FINPUT handle other keys
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      gEditMode = EDIT_MODE_NONE;
      gRedrawScreen = true;
      return true;
    case KEY_MENU:
      editLootField(gEditLootIndex, gEditField);
      return true;
    case KEY_UP:
      if (gEditField > 0)
        gEditField--;
      return true;
    case KEY_DOWN:
      if (gEditField < EDIT_FIELD_COUNT - 1)
        gEditField++;
      return true;
    default:
      break;
    }
  }

  return false;
}

void LOOTLIST_update() {
  // Nothing to update currently
}

static Menu lootMenu = {"Loot", .render_item = renderItem, .action = action};

static void initMenu() {
  lootMenu.num_items = LOOT_Size();
  lootMenu.itemHeight = shortList ? MENU_ITEM_H : MENU_ITEM_H_LARGER;
  MENU_Init(&lootMenu);
}

void LOOTLIST_render(void) {
  if (gFInputActive) {
    FINPUT_render();
    return;
  }

  if (gEditMode != EDIT_MODE_NONE) {
    renderEditMode();
    return;
  }

  MENU_Render();
}

void LOOTLIST_init(void) {
  prevScanMode = SCAN_GetMode();
  SCAN_SetMode(SCAN_MODE_SINGLE);
  initMenu();
  sortType = SORT_F;
  sort(SORT_LOT);
  if (LOOT_Size()) {
    tuneToLoot(LOOT_Item(menuIndex), false);
  }
}

bool LOOTLIST_key(KEY_Code_t key, Key_State_t state) {
  // Handle FINPUT (for channel save, edit mode, etc.)
  if (gFInputActive) {
    if (key == KEY_EXIT && state == KEY_RELEASED) {
      gFInputActive = false;
      // If we were in edit mode FINPUT, return to edit mode active
      if (gEditMode == EDIT_MODE_FINPUT) {
        gEditMode = EDIT_MODE_ACTIVE;
      }
      gRedrawScreen = true;
      return true;
    }
    return FINPUT_key(key, state);
  }

  // Handle edit mode
  if (gEditMode != EDIT_MODE_NONE) {
    if (editModeKey(key, state)) {
      return true;
    }
    // Let FINPUT handle input when in FINPUT mode
    if (gEditMode == EDIT_MODE_FINPUT) {
      return FINPUT_key(key, state);
    }
    return false;
  }

  if (MENU_HandleInput(key, state)) {
    return true;
  }

  return false;
}
