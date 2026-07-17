#include "keymap.h"
#include "../apps/apps.h"
#include "../driver/keyboard.h"
#include "../helper/keymap.h"
#include "../helper/measurements.h"
#include "../helper/menu.h"
#include "../radio.h"
#include "../settings.h"
#include "graphics.h"
#include "statusline.h"
#include <string.h>

bool gKeymapActive;

static const uint8_t ITEM_H = 19;

typedef enum { KM_KEYS, KM_ACTIONS } KmState;
static KmState kmState;

static KEY_Code_t currentKey;
static AppAction_t *currentAction;

// pending param редактируется прямо в submenu
static int16_t pendingParam;
static KeyAction pendingAction;

static Menu menu = {
    .itemHeight = ITEM_H,
};
static Menu submenu = {
    .itemHeight = MENU_ITEM_H,
    .title = "Select Action",
    .x = 0,
    .width = LCD_WIDTH,
};

// --- helpers ---

static ParamType actionToParamType(KeyAction a) {
  switch (a) {
  case KA_STEP:
    return PARAM_STEP;
  case KA_BW:
    return PARAM_BANDWIDTH;
  case KA_GAIN:
    return PARAM_GAIN;
  case KA_POWER:
    return PARAM_POWER;
  case KA_MODULATION:
    return PARAM_MODULATION;
  case KA_SQUELCH:
    return PARAM_SQUELCH_VALUE;
  case KA_VOLUME:
    return PARAM_VOLUME;
  case KA_AFC:
    return PARAM_AFC;
  case KA_DEV:
    return PARAM_DEV;
  case KA_FILTER:
    return PARAM_FILTER;
  case KA_SCRAMBLER:
    return PARAM_SCRAMBLER;
  case KA_RADIO:
    return PARAM_RADIO;
  case KA_XTAL:
    return PARAM_XTAL;
  case KA_OFFSET:
    return PARAM_TX_OFFSET;
  default:
    return (ParamType)-1;
  }
}

static bool actionHasParam(KeyAction a) {
  return a == KA_APP_LAUNCH || actionToParamType(a) != (ParamType)-1;
}

static void resetPendingParam(KeyAction a) {
  if (a == pendingAction)
    return;
  pendingAction = a;
  if (a == currentAction->action && currentAction->param != KA_PARAM_DEFAULT) {
    pendingParam = currentAction->param;
  } else if (a == KA_APP_LAUNCH) {
    pendingParam = 0;
  } else {
    pendingParam = 1; // шаг +1 по умолчанию
  }
}

// --- menu render callbacks ---

static void fmtAction(KeyAction a, int16_t p, char *buf, uint8_t sz) {
  if (p != KA_PARAM_DEFAULT && actionHasParam(a)) {
    if (a == KA_APP_LAUNCH) {
      int16_t cnt = (int16_t)ARRAY_SIZE(apps);
      snprintf(buf, sz, "App:%s", (p >= 0 && p < cnt) ? apps[p].name : "?");
    } else {
      snprintf(buf, sz, "%s%+d", KA_NAMES[a], (int)p);
    }
  } else {
    strncpy(buf, KA_NAMES[a], sz - 1);
    buf[sz - 1] = '\0';
  }
}

static void renderItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * ITEM_H;
  KeyAction clickAct = gCurrentKeymap.click[index].action;
  KeyAction longAct = gCurrentKeymap.long_press[index].action;
  int16_t clickP = gCurrentKeymap.click[index].param;
  int16_t longP = gCurrentKeymap.long_press[index].param;

  char clickBuf[14], longBuf[14];
  fmtAction(clickAct, clickP, clickBuf, sizeof(clickBuf));
  fmtAction(longAct, longP, longBuf, sizeof(longBuf));

  PrintMediumEx(13, y + 8, POS_L, C_INVERT, "%s: %s", KEY_NAMES[index],
                clickBuf);
  PrintMediumEx(13, y + 8 + 8, POS_L, C_INVERT, "%s L: %s", KEY_NAMES[index],
                longBuf);
}

static void renderSubItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * MENU_ITEM_H;

  const char *name = KA_NAMES[index];
  char buf[18];
  if (strlen(name) > 16) {
    strncpy(buf, name, 16);
    buf[16] = '~';
    buf[17] = '\0';
  } else {
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  }

  PrintMediumEx(13, y + 8, POS_L, C_FILL, "  %s", buf);

  // значение param справа — только для выбранного пункта с параметром
  if (index == submenu.i && actionHasParam((KeyAction)index)) {
    resetPendingParam((KeyAction)index);
    if ((KeyAction)index == KA_APP_LAUNCH) {
      int16_t cnt = (int16_t)ARRAY_SIZE(apps);
      const char *appName = (pendingParam >= 0 && pendingParam < cnt)
                                ? apps[pendingParam].name
                                : "?";
      PrintSmallEx(LCD_WIDTH - 4, y + 8, POS_R, C_FILL, "%s", appName);
    } else {
      PrintSmallEx(LCD_WIDTH - 4, y + 8, POS_R, C_FILL, "%+d",
                   (int)pendingParam);
    }
  }
}

// --- menu action callbacks ---

static bool action(uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (state == KEY_PRESSED)
    return false;
  if (key == KEY_UP || key == KEY_DOWN)
    return false;
  if (key == KEY_STAR)
    return true;

  if (state == KEY_RELEASED && key == KEY_EXIT) {
    KEYMAP_Hide();
    return true;
  }

  if (key == KEY_MENU && (state == KEY_RELEASED || state == KEY_LONG_PRESSED)) {
    bool longPress = (state == KEY_LONG_PRESSED);
    currentKey = index;
    currentAction = longPress ? &gCurrentKeymap.long_press[index]
                              : &gCurrentKeymap.click[index];
    submenu.i = currentAction->action;
    if (submenu.i >= KA_COUNT)
      submenu.i = 0;
    pendingAction = KA_NONE; // форсируем reset при входе
    resetPendingParam((KeyAction)submenu.i);
    kmState = KM_ACTIONS;
    MENU_Init(&submenu);
    return true;
  }

  // быстрый переход к нужной кнопке
  menu.i = key;
  return true;
}

static bool subAction(uint16_t index, KEY_Code_t key, Key_State_t state) {
  (void)index;

  // навигация: сбрасываем pendingParam для нового пункта
  if (key == KEY_UP || key == KEY_DOWN) {
    // index уже обновлён в menu; сбрасываем через resetPendingParam
    pendingAction = KA_NONE;
    // KA_VOX недоступен для назначения — не переработан, шагаем дальше
    if ((KeyAction)submenu.i == KA_VOX) {
      submenu.i = IncDecU(submenu.i, 0, submenu.num_items, key == KEY_DOWN);
    }
    return false; // меню само обработало навигацию
  }

  bool isNav = (state == KEY_RELEASED || state == KEY_LONG_PRESSED ||
                state == KEY_LONG_PRESSED_CONT);

  // STAR/F: изменяем pendingParam прямо в списке
  if (isNav && (key == KEY_STAR || key == KEY_F) &&
      actionHasParam((KeyAction)submenu.i)) {
    resetPendingParam((KeyAction)submenu.i);
    bool up = (key == KEY_STAR);
    if ((KeyAction)submenu.i == KA_APP_LAUNCH) {
      int16_t cnt = (int16_t)ARRAY_SIZE(apps);
      if (up) {
        pendingParam = (pendingParam + 1 < cnt) ? pendingParam + 1 : 0;
      } else {
        pendingParam = (pendingParam > 0) ? pendingParam - 1 : cnt - 1;
      }
    } else {
      pendingParam += up ? 1 : -1;
      if (pendingParam == 0)
        pendingParam = up ? 1 : -1; // пропускаем 0
    }
    gRedrawScreen = true;
    gLastRender = 0;
    return true;
  }

  if (state != KEY_RELEASED)
    return false;

  if (key == KEY_EXIT) {
    MENU_Deinit();
    kmState = KM_KEYS;
    MENU_Init(&menu);
    return true;
  }

  if (key == KEY_MENU) {
    KeyAction newAct = (KeyAction)submenu.i;
    currentAction->action = newAct;
    currentAction->param =
        actionHasParam(newAct) ? pendingParam : KA_PARAM_DEFAULT;
    MENU_Deinit();
    kmState = KM_KEYS;
    MENU_Init(&menu);
    KEYMAP_Save();
    return true;
  }

  return false;
}

// --- public API ---

void KEYMAP_Render(void) { MENU_Render(); }

bool KEYMAP_Key(KEY_Code_t key, Key_State_t state) {
  MENU_HandleInput(key, state);
  return true;
}

void KEYMAP_Show(void) {
  menu.num_items = KEY_COUNT;
  menu.render_item = renderItem;
  menu.action = action;
  menu.items = NULL;

  submenu.num_items = KA_COUNT;
  submenu.render_item = renderSubItem;
  submenu.action = subAction;
  submenu.items = NULL;
  submenu.i = 0;
  submenu.y = MENU_Y;
  submenu.height = LCD_HEIGHT - MENU_Y;

  kmState = KM_KEYS;
  gKeymapActive = true;
  MENU_Init(&menu);
}

void KEYMAP_Hide(void) {
  KEYMAP_Save();
  gKeymapActive = false;
  kmState = KM_KEYS;
  MENU_Deinit();
}
