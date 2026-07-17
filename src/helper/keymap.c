#include "keymap.h"
#include "../apps/apps.h"
#include "../driver/lfs.h"
#include "../external/printf/printf.h"
#include "storage.h"
#include <string.h>

AppKeymap_t gCurrentKeymap;

const char *KA_NAMES[] = {
    [KA_NONE] = "NONE",

    // Приложения
    [KA_APP_LAUNCH] = "Run App",
    [KA_EXIT_APP]   = "Exit App",

    // Частота и навигация
    [KA_FREQ_INPUT]   = "Freq Input",
    [KA_VFO_MODE]     = "VFO Mode",
    [KA_NEXT_VFO]     = "Next VFO",
    [KA_NEXT_CH]      = "Next Ch",
    [KA_PREV_CH]      = "Prev Ch",
    [KA_TUNE_TO_LOOT] = "Tune Loot",

    // Параметры радио
    [KA_STEP]      = "Step",
    [KA_BW]        = "BW",
    [KA_MODULATION]= "Mod",
    [KA_SQUELCH]   = "SQL",
    [KA_GAIN]      = "Gain",
    [KA_POWER]     = "Power",
    [KA_VOLUME]    = "Volume",
    [KA_AFC]       = "AFC",
    [KA_DEV]       = "Dev",
    [KA_FILTER]    = "Filter",
    [KA_SCRAMBLER] = "Scrambler",
    [KA_RADIO]     = "Radio",
    [KA_XTAL]      = "Xtal",
    [KA_OFFSET]    = "Offset",
    [KA_OFFSET_DIR]= "Offset Dir",

    // Скан и списки
    [KA_LOOTLIST]       = "Lootlist",
    [KA_CH_LIST]        = "Ch List",
    [KA_MULTIWATCH]     = "Multiwatch",
    [KA_BLACKLIST_LAST] = "BL Last",
    [KA_WHITELIST_LAST] = "WL Last",
    [KA_NEXT_BLACKLIST] = "Next BL",
    [KA_NEXT_WHITELIST] = "Next WL",
    [KA_CLEAR_LOOT]     = "Clear Loot",
    [KA_SAVE_LOOT_CH]   = "Save Loot",

    // TX / PTT
    [KA_TX]   = "TX",
    [KA_PTT]  = "PTT",
    [KA_VOX]  = "VOX (n/a)",
    [KA_MONI] = "Monitor",

    // Отображение
    [KA_RSSI]          = "RSSI",
    [KA_RSSI_GRAPH]    = "RSSI Graph",
    [KA_ALWAYS_RSSI]   = "Always RSSI",
    [KA_LEVEL_DISPLAY] = "Level Disp",
    [KA_GRAPH_UNIT]    = "Graph Unit",
    [KA_VFO_MENU]      = "VFO Menu",
    [KA_RADIO_SETTINGS]= "Radio Regs",
    [KA_PRO_MODE]      = "Pro Mode",

    // Полоса / диапазон
    [KA_BANDS]       = "Bands",
    [KA_CHANNELS]    = "Channels",
    [KA_BAND_UP]     = "Band Up",
    [KA_BAND_DOWN]   = "Band Down",
    [KA_ZOOM_IN]     = "Zoom In",
    [KA_ZOOM_OUT]    = "Zoom Out",
    [KA_RANGE_INPUT] = "Range In",

    // Быстрые настройки
    [KA_BL]          = "Backlight",
    [KA_BL_MAX]      = "BL Max",
    [KA_BL_MIN]      = "BL Min",
    [KA_CONTRAST]    = "Contrast",
    [KA_BEEP]        = "Beep",
    [KA_INVERT_BTNS] = "Inv Btns",
    [KA_FLASHLIGHT]  = "Flashlight",

    // Прочее
    [KA_FASTMENU1] = "Fast Menu 1",
    [KA_FASTMENU2] = "Fast Menu 2",
};

static char keymapDir[16];
static char keymapFile[32];

void KEYMAP_Load(void) {
  snprintf(keymapDir,  sizeof(keymapDir),  "/%s", apps[gCurrentApp].name);
  snprintf(keymapFile, sizeof(keymapFile), "%s/keymap.key", keymapDir);

  struct lfs_info info;
  if (lfs_stat(&gLfs, keymapDir, &info) < 0) {
    lfs_mkdir(&gLfs, keymapDir);
  }
  if (!lfs_file_exists(keymapFile)) {
    STORAGE_INIT(keymapFile, AppKeymap_t, 1);
  }

  STORAGE_LOAD(keymapFile, 1, &gCurrentKeymap);
}

void KEYMAP_Save(void) {
  STORAGE_SAVE(keymapFile, 1, &gCurrentKeymap);
}
