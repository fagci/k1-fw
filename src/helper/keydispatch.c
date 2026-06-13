#include "keydispatch.h"
#include "../apps/apps.h"
#include "../board.h"
#include "../driver/backlight.h"
#include "../helper/keymap.h"
#include "../helper/lootlist.h"
#include "../helper/menu.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../helper/screenshot.h"
#include "../helper/vfomenu.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/chlist.h"
#include "../ui/finput.h"
#include "../ui/keymap.h"
#include "../ui/lootlist.h"
#include "../ui/spectrum.h"
#include "../ui/textinput.h"

// Keylock / screenshot / spec-keys passthrough
static bool checkKeylock(KEY_State_t state, KEY_Code_t key) {
  if (state == KEY_LONG_PRESSED && key == KEY_F) {
    gSettings.keylock = !gSettings.keylock;
    SETTINGS_Save();
    return true;
  }
  if (gSettings.keylock && state == KEY_LONG_PRESSED && key == KEY_8) {
    captureScreen();
    return true;
  }

  bool isSpecialKey = key == KEY_PTT || key == KEY_SIDE1 || key == KEY_SIDE2;
  return gSettings.keylock && (gSettings.pttLock || !isSpecialKey);
}

// Шаговый inc/dec: param > 0 = вверх N раз, param < 0 = вниз N раз,
// KA_PARAM_DEFAULT = 1 раз defaultUp
static void setOrInc(VFOContext *ctx, AppAction_t act, ParamType pt,
                     bool defaultUp) {
  if (act.param == KA_PARAM_DEFAULT) {
    RADIO_IncDecParam(ctx, pt, defaultUp, true);
    return;
  }
  bool up = act.param > 0;
  int16_t n = up ? act.param : -act.param;
  for (int16_t i = 0; i < n; i++) {
    RADIO_IncDecParam(ctx, pt, up, i == n - 1);
  }
}

static bool keyAction(AppAction_t act) {
  VFOContext *ctx = &RADIO_GetCurrentVFO(gRadioState)->context;

  switch (act.action) {
  case KA_FLASHLIGHT:
    BOARD_FlashlightToggle();
    return true;

  case KA_STEP:
    setOrInc(ctx, act, PARAM_STEP, true);
    return true;
  case KA_BW:
    setOrInc(ctx, act, PARAM_BANDWIDTH, true);
    return true;
  case KA_GAIN:
    setOrInc(ctx, act, PARAM_GAIN, true);
    return true;
  case KA_POWER:
    setOrInc(ctx, act, PARAM_POWER, true);
    return true;
  case KA_MODULATION:
    setOrInc(ctx, act, PARAM_MODULATION, true);
    return true;
  case KA_SQUELCH:
    setOrInc(ctx, act, PARAM_SQUELCH_VALUE, true);
    return true;
  case KA_OFFSET:
    setOrInc(ctx, act, PARAM_TX_OFFSET, true);
    return true;
  case KA_OFFSET_DIR:
    RADIO_IncDecParam(ctx, PARAM_TX_OFFSET_DIR, true, true);
    return true;
  case KA_RADIO:
    setOrInc(ctx, act, PARAM_RADIO, true);
    return true;
  case KA_FILTER:
    setOrInc(ctx, act, PARAM_FILTER, true);
    return true;
  case KA_AFC:
    setOrInc(ctx, act, PARAM_AFC, true);
    return true;
  case KA_DEV:
    setOrInc(ctx, act, PARAM_DEV, true);
    return true;
  case KA_XTAL:
    setOrInc(ctx, act, PARAM_XTAL, true);
    return true;
  case KA_SCRAMBLER:
    setOrInc(ctx, act, PARAM_SCRAMBLER, true);
    return true;
  case KA_VOLUME:
    setOrInc(ctx, act, PARAM_VOLUME, true);
    return true;

  case KA_RSSI:
    gShowAllRSSI = !gShowAllRSSI;
    return true;
  case KA_RSSI_GRAPH:
    gSettings.showLevelInVFO = !gSettings.showLevelInVFO;
    SETTINGS_DelayedSave();
    return true;
  case KA_ALWAYS_RSSI:
    gSettings.alwaysRssi = !gSettings.alwaysRssi;
    SETTINGS_DelayedSave();
    return true;
  case KA_GRAPH_UNIT:
    SP_NextGraphUnit(true);
    return true;
  case KA_LEVEL_DISPLAY:
    gSettings.showLevelInVFO = !gSettings.showLevelInVFO;
    SETTINGS_DelayedSave();
    return true;
  case KA_VFO_MENU:
    VFOMENU_Key(KEY_F, KEY_RELEASED);
    return true;
  case KA_RADIO_SETTINGS:
    REGSMENU_Key(KEY_0, KEY_RELEASED);
    return true;
  case KA_PRO_MODE:
    gSettings.iAmPro = !gSettings.iAmPro;
    SETTINGS_Save();
    return true;

  case KA_MONI:
    gMonitorMode = !gMonitorMode;
    return true;
  case KA_TX:
    RADIO_ToggleTX(ctx, true);
    return true;
  case KA_PTT:
    RADIO_ToggleTX(ctx, !ctx->tx_state.is_active);
    return true;
  case KA_VOX:
    return true;

  case KA_FREQ_INPUT:
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(NULL);
    return true;

  case KA_VFO_MODE: {
    uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);
    RADIO_SaveCurrentVFO(gRadioState);
    RADIO_ToggleVFOMode(gRadioState, vfoN);
    return true;
  }

  case KA_NEXT_CH:
    RADIO_NextChannel(true);
    return true;
  case KA_PREV_CH:
    RADIO_NextChannel(false);
    return true;

  case KA_NEXT_VFO: {
    uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);
    RADIO_SaveCurrentVFO(gRadioState);
    RADIO_SwitchVFO(gRadioState, IncDecU(vfoN, 0, gRadioState->num_vfos, true));
    return true;
  }

  case KA_TUNE_TO_LOOT:
    if (gLastActiveLoot) {
      RADIO_SetParam(ctx, PARAM_FREQUENCY, gLastActiveLoot->f, true);
      RADIO_ApplySettings(ctx);
    }
    return true;

  case KA_LOOTLIST:
    LOOTLIST_init();
    gLootlistActive = true;
    return true;
  case KA_CH_LIST:
    CHLIST_init();
    gChlistActive = true;
    return true;
  case KA_MULTIWATCH:
    RADIO_ToggleMultiwatch(gRadioState, !gRadioState->multiwatch_enabled);
    return true;
  case KA_BLACKLIST_LAST:
    LOOT_BlacklistLast();
    return true;
  case KA_WHITELIST_LAST:
    LOOT_WhitelistLast();
    return true;
  case KA_NEXT_BLACKLIST:
    SCAN_NextBlacklist();
    return true;
  case KA_NEXT_WHITELIST:
    SCAN_NextWhitelist();
    return true;
  case KA_CLEAR_LOOT:
    LOOT_Clear();
    return true;
  case KA_SAVE_LOOT_CH:
    return true;

  case KA_BANDS:
  case KA_CHANNELS:
  case KA_BAND_UP:
  case KA_BAND_DOWN:
  case KA_ZOOM_IN:
  case KA_ZOOM_OUT:
  case KA_RANGE_INPUT:
    return true;

  case KA_APP_LAUNCH:
    if (act.param != KA_PARAM_DEFAULT && act.param >= 0 &&
        act.param < (int16_t)ARRAY_SIZE(apps)) {
      APPS_run((AppType_t)act.param);
    }
    return true;
  case KA_EXIT_APP:
    APPS_exit();
    return true;

  case KA_FASTMENU1:
  case KA_FASTMENU2:
    return true;

  case KA_BL:
  case KA_BL_MAX:
  case KA_BL_MIN:
  case KA_CONTRAST:
  case KA_BEEP:
  case KA_INVERT_BTNS:
    return true;

  case KA_NONE:
  default:
    return false;
  }
}

#define HANDLE_OVERLAY(active, fn, k, s)                                       \
  if (active && fn(k, s)) {                                                    \
    gRedrawScreen = true;                                                      \
    gLastRender = 0;                                                           \
    return;                                                                    \
  }

void KEYDISPATCH_onKey(KEY_Code_t key, KEY_State_t state) {
  BACKLIGHT_TurnOn();

  if (gCurrentApp != APP_SETTINGS && checkKeylock(state, key)) {
    gRedrawScreen = true;
    return;
  }

  HANDLE_OVERLAY(gFInputActive, FINPUT_key, key, state)
  HANDLE_OVERLAY(gTextInputActive, TEXTINPUT_key, key, state)
  HANDLE_OVERLAY(gLootlistActive, LOOTLIST_key, key, state)
  HANDLE_OVERLAY(gChlistActive, CHLIST_key, key, state)
  HANDLE_OVERLAY(gKeymapActive, KEYMAP_Key, key, state)

  if (state == KEY_LONG_PRESSED && key == KEY_STAR) {
    KEYMAP_Show();
  } else if (state == KEY_LONG_PRESSED &&
             gCurrentKeymap.long_press[key].action != KA_NONE) {
    if (!keyAction(gCurrentKeymap.long_press[key])) {
      goto apps;
    }
  } else if (state == KEY_RELEASED &&
             gCurrentKeymap.click[key].action != KA_NONE) {
    if (!keyAction(gCurrentKeymap.click[key])) {
      goto apps;
    }
  } else {
  apps:
    if (APPS_key(key, state) || (MENU_IsActive() && key != KEY_EXIT)) {
    } else if (key == KEY_MENU) {
      if (state == KEY_LONG_PRESSED) {
        APPS_run(APP_SETTINGS);
      } else if (state == KEY_RELEASED) {
        APPS_run(APP_APPS_LIST);
      }
    } else if (key == KEY_EXIT && state == KEY_RELEASED) {
      APPS_exit();
    }
  }

  gRedrawScreen = true;
  gLastRender = 0;
}
