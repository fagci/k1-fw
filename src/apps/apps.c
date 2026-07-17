#include "analyser.h"
#include "apps.h"
#include "../driver/st7565.h"
#include "../driver/uart.h"
#include "../helper/keymap.h"
#include "../helper/menu.h"
#include "../settings.h"
#include "../ui/chlist.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "about.h"
#include "appslist.h"
#include "cmdedit.h"
#include "cmdscan.h"
#include "fc.h"
#include "files.h"
#include "messenger.h"
#include "sqviewer.h"
#include "scaner.h"
#include "settings.h"
#include "vfo1.h"

#define APPS_STACK_SIZE 8

AppType_t gCurrentApp = APP_NONE;
char gOpenedFile[64] = {0};

static AppType_t loadedVfoApp = APP_NONE;

static AppType_t appsStack[APPS_STACK_SIZE] = {APP_NONE};
static int8_t stackIndex = -1;

static bool pushApp(AppType_t app) {
  if (stackIndex < APPS_STACK_SIZE - 1) {
    appsStack[++stackIndex] = app;
  } else {
    for (uint8_t i = 1; i < APPS_STACK_SIZE; ++i) {
      appsStack[i - 1] = appsStack[i];
    }
    appsStack[stackIndex] = app;
  }
  return true;
}

static AppType_t popApp(void) {
  if (stackIndex > 0) {
    return appsStack[stackIndex--]; // Do not care about existing value
  }
  return appsStack[stackIndex];
}

AppType_t APPS_Peek(void) {
  if (stackIndex >= 0) {
    return appsStack[stackIndex];
  }
  return APP_NONE;
}

const AppType_t appsAvailableToRun[RUN_APPS_COUNT] = {
    APP_VFO1,    //
    APP_SCANER,  //
    APP_ANALYSER, //
    APP_CMDSCAN, //
    APP_FC,      //
    APP_MESSENGER, //
    APP_FILES,     //
    APP_ABOUT,     //
};

const App apps[APPS_COUNT] = {
    [APP_NONE] = {"None"},
    [APP_SETTINGS] = {"Settings", SETTINGS_init, NULL, SETTINGS_render,
                      SETTINGS_key, SETTINGS_deinit},
    [APP_APPS_LIST] = {"Run app", APPSLIST_init, NULL, APPSLIST_render,
                       APPSLIST_key, NULL},
    [APP_SCANER] = {"Scanner", SCANER_init, SCANER_update, SCANER_render,
                    SCANER_key, SCANER_deinit, true},
    [APP_FC] = {"FC", FC_init, FC_update, FC_render, FC_key, FC_deinit, true},
    [APP_VFO1] = {"1 VFO", VFO1_init, VFO1_update, VFO1_render, VFO1_key, NULL,
                  true},
    [APP_CMDSCAN] = {"CMD Scan", CMDSCAN_init, CMDSCAN_update, CMDSCAN_render,
                     CMDSCAN_key, CMDSCAN_deinit, true},
    [APP_CMDEDIT] = {"CMD Scan", CMDEDIT_init, NULL, CMDEDIT_render,
                     CMDEDIT_key, NULL},
    [APP_ANALYSER] = {"Analyzer", ANALYSER_init, ANALYSER_update, ANALYSER_render,
                     ANALYSER_key, ANALYSER_deinit, true},
    [APP_SQVIEWER] = {"SQ Editor", SQVIEWER_init, NULL, SQVIEWER_render,
                     SQVIEWER_key, SQVIEWER_deinit, false},
    [APP_CHLIST] = {"CH Editor", CHLIST_init, NULL, CHLIST_render,
                     CHLIST_key, CHLIST_deinit, false},
    [APP_MESSENGER] = {"MESSENGER", MESSENGER_init, MESSENGER_update,
                       MESSENGER_render, MESSENGER_key, MESSENGER_deinit, true},
    [APP_FILES] = {"Files", FILES_init, NULL, FILES_render, FILES_key, NULL},
    [APP_ABOUT] = {"ABOUT", NULL, NULL, ABOUT_Render, NULL, NULL},
};

bool APPS_key(KEY_Code_t Key, KEY_State_t state) {
  if (apps[gCurrentApp].key) {
    return apps[gCurrentApp].key(Key, state);
  }
  return false;
}

void APPS_init(AppType_t app) {

  STATUSLINE_SetText("%s", apps[app].name);
  gRedrawScreen = true;

  LogC(LOG_C_YELLOW, "[APP] Init %s", apps[gCurrentApp].name);
  if (apps[app].init) {
    apps[app].init();
  }
}

void APPS_update(void) {
  if (apps[gCurrentApp].update) {
    apps[gCurrentApp].update();
  }
}

void APPS_render(void) {
  if (apps[gCurrentApp].render) {
    apps[gCurrentApp].render();
  }
}

void APPS_deinit(void) {
  LogC(LOG_C_YELLOW, "[APP] Deinit %s", apps[gCurrentApp].name);
  MENU_Deinit();
  if (apps[gCurrentApp].deinit) {
    apps[gCurrentApp].deinit();
  }
}

RadioState radioState;

static void APPS_runInternal(AppType_t app);

void APPS_run(AppType_t app) {
  gOpenedFile[0] = '\0';  // Clear opened file for normal app launch
  APPS_runInternal(app);
}

void APPS_runWithFile(AppType_t app, const char *filename) {
  strncpy(gOpenedFile, filename, sizeof(gOpenedFile) - 1);
  gOpenedFile[sizeof(gOpenedFile) - 1] = '\0';
  APPS_runInternal(app);
}

static void APPS_runInternal(AppType_t app) {
  if (appsStack[stackIndex] == app)
    return;

  // Пока gRadioState в памяти уже актуален (принадлежал предыдущему
  // radio-приложению), reload из flash сразу после save — чистый
  // no-op ценой долгого I/O по всем VFO. Пропускаем его.
  bool wasRadioStateFresh = gRadioState && apps[gCurrentApp].needsRadioState;

  APPS_deinit();
  pushApp(app);
  gCurrentApp = app;

  if (apps[gCurrentApp].needsRadioState) {
    if (gRadioState) {
      // Save ALL VFOs from current app before switching
      RADIO_SaveAllVFOs(gRadioState);
    }
    if (!gRadioState) {
      // Один раз: выделяем структуру и настраиваем
      gRadioState = &radioState;
      RADIO_InitState(gRadioState, MAX_VFOS);
      KEYMAP_Load();
      RADIO_ToggleMultiwatch(gRadioState, gSettings.mWatch);
    }
    if (!wasRadioStateFresh) {
      // Загружаем актуальное состояние из хранилища — оно могло
      // измениться извне (редактор каналов, файлы и т.п.)
      RADIO_LoadVFOs(gRadioState);
    }
  }

  APPS_init(app);
}

void APPS_runManual(AppType_t app) { APPS_run(app); }

bool APPS_exit(void) {
  if (stackIndex == 0) {
    return false;
  }

  bool wasRadioStateFresh = gRadioState && apps[gCurrentApp].needsRadioState;

  // Save VFO state before exiting current app
  if (wasRadioStateFresh) {
    RADIO_SaveAllVFOs(gRadioState);
  }

  APPS_deinit();
  AppType_t app = popApp();
  gCurrentApp = APPS_Peek();

  // Load VFO state for the app we're returning to, если оно ещё не
  // актуально в памяти (см. комментарий в APPS_runInternal)
  if (apps[gCurrentApp].needsRadioState && gRadioState && !wasRadioStateFresh) {
    RADIO_LoadVFOs(gRadioState);
  }

  APPS_init(gCurrentApp);

  STATUSLINE_SetText("%s", apps[gCurrentApp].name);
  gRedrawScreen = true;
  // }
  return true;
}
