#include "settings.h"
#include "../driver/backlight.h"
#include "../driver/battery.h"
#include "../driver/bk4829.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../misc.h"
#include "../settings.h"
#include "../ui/finput.h"
#include "apps.h"
// #include "finput.h"

static void getValS(const MenuItem *item, char *buf, uint8_t buf_size) {
  sprintf(buf, "%s", SETTINGS_GetValueString(item->setting));
}

static void updateValS(const MenuItem *item, bool up) {
  SETTINGS_IncDecValue(item->setting, up);

  // Apply EQ settings immediately
  switch (item->setting) {
  case SETTING_AF_RX_300:
    RADIO_ApplyAFResponse(false, false);
    break;
  case SETTING_AF_RX_3K:
    RADIO_ApplyAFResponse(false, true);
    break;
  case SETTING_AF_TX_300:
    RADIO_ApplyAFResponse(true, false);
    break;
  case SETTING_AF_TX_3K:
    RADIO_ApplyAFResponse(true, true);
    break;
  default:
    break;
  }
}

static void doCalibrate(uint32_t v, uint32_t _) {
  (void)_;
  SETTINGS_SetValue(SETTING_BATTERYCALIBRATION, BATTERY_GetCal(v * 100));
}
static void setUpConv(uint32_t v, uint32_t _) {
  (void)_;
  SETTINGS_SetValue(SETTING_UPCONVERTER, v);
}
static void setFcorr(uint32_t v, uint32_t _) {
  (void)_;
  SETTINGS_SetValue(SETTING_FREQ_CORRECTION, v);
  ctx->dirty[PARAM_FREQUENCY] = true;
  RADIO_ApplySettings(ctx);
}

static bool calibrate(const MenuItem *item, KEY_Code_t key, Key_State_t state) {
  (void)item;

  if (state != KEY_RELEASED || key != KEY_MENU) {
    return false;
  }

  uint32_t currentVoltage =
      BATTERY_GetPreciseVoltage(SETTINGS_GetValue(SETTING_BATTERYCALIBRATION));

  gFInputValue1 = currentVoltage / 100;
  gFInputCallback = doCalibrate;
  FINPUT_setup(500, 860, UNIT_VOLTS, false);
  FINPUT_init();
  gFInputActive = true;
  return true;
}
static bool fcorr(const MenuItem *item, KEY_Code_t key, Key_State_t state) {
  (void)item;

  if (state != KEY_RELEASED || key != KEY_MENU) {
    return false;
  }

  FINPUT_setup(0, 50000000, UNIT_MHZ, false);
  FINPUT_Show(setFcorr);
  return true;
}
static bool upconv(const MenuItem *item, KEY_Code_t key, Key_State_t state) {
  (void)item;

  if (state != KEY_RELEASED || key != KEY_MENU) {
    return false;
  }

  FINPUT_setup(0, 50000000, UNIT_MHZ, false);
  FINPUT_Show(setUpConv);
  return true;
}

// SQL submenu
static const MenuItem sqlMenuItems[] = {
    {"Open t", SETTING_SQLOPENTIME, getValS, updateValS},
    {"Close t", SETTING_SQLCLOSETIME, getValS, updateValS},
};

static Menu sqlMenu = {.title = "SQL",
                       .items = sqlMenuItems,
                       .num_items = ARRAY_SIZE(sqlMenuItems)};

// Scan submenu
static const MenuItem scanMenuItems[] = {
    {"SQL", .submenu = &sqlMenu},
    {"FC t", SETTING_FCTIME, getValS, updateValS},
    {"Listen t/o", SETTING_SQOPENEDTIMEOUT, getValS, updateValS},
    {"Stay t", SETTING_SQCLOSEDTIMEOUT, getValS, updateValS},
    {"Skip X_X", SETTING_SKIPGARBAGEFREQUENCIES, getValS, updateValS},
};

static Menu scanMenu = {.title = "Scan",
                        .items = scanMenuItems,
                        .num_items = ARRAY_SIZE(scanMenuItems)};

// Display submenu
static const MenuItem displayMenuItems[] = {
    {"Contrast", SETTING_CONTRAST, getValS, updateValS},
    {"BL max", SETTING_BRIGHTNESS_H, getValS, updateValS},
    {"BL min", SETTING_BRIGHTNESS_L, getValS, updateValS},
    {"BL time", SETTING_BACKLIGHT, getValS, updateValS},
    {"BL SQL mode", SETTING_BACKLIGHTONSQUELCH, getValS, updateValS},
    {"CH display", SETTING_CHDISPLAYMODE, getValS, updateValS},
    {"Level in VFO", SETTING_SHOWLEVELINVFO, getValS, updateValS},
    {"Always RSSI", SETTING_ALWAYS_RSSI, getValS, updateValS},
};

static Menu displayMenu = {.title = "Display",
                           .items = displayMenuItems,
                           .num_items = ARRAY_SIZE(displayMenuItems)};

// Radio submenu
static const MenuItem radioMenuItems[] = {
    {"DTMF decode", SETTING_DTMFDECODE, getValS, updateValS},
    {"Filter bound", SETTING_BOUND240_280, getValS, updateValS},
    {"SI power off", SETTING_SI4732POWEROFF, getValS, updateValS},
    {"STE", SETTING_STE, getValS, updateValS},
    {"Tone local", SETTING_TONELOCAL, getValS, updateValS},
    {"Roger", SETTING_ROGER, getValS, updateValS},
    {"Multiwatch", SETTING_MULTIWATCH, getValS, updateValS},
    {"Mic", SETTING_MIC, getValS, updateValS},
    {"Freq corr", SETTING_FREQ_CORRECTION, getValS, updateValS,
     .action = fcorr},
    {"Upconv", SETTING_UPCONVERTER, getValS, .action = upconv},
};

static Menu radioMenu = {.title = "Radio",
                         .items = radioMenuItems,
                         .num_items = ARRAY_SIZE(radioMenuItems)};

// Equalizer submenu
static const MenuItem eqMenuItems[] = {
    {"RX 300Hz", SETTING_AF_RX_300, getValS, updateValS},
    {"RX 3kHz", SETTING_AF_RX_3K, getValS, updateValS},
    {"TX 300Hz", SETTING_AF_TX_300, getValS, updateValS},
    {"TX 3kHz", SETTING_AF_TX_3K, getValS, updateValS},
};

static Menu eqMenu = {.title = "Equalizer",
                      .items = eqMenuItems,
                      .num_items = ARRAY_SIZE(eqMenuItems)};

// Battery submenu
static const MenuItem batMenuItems[] = {
    {"Bat type", SETTING_BATTERYTYPE, getValS, updateValS},
    {"Bat style", SETTING_BATTERYSTYLE, getValS, updateValS},
    {"BAT cal", SETTING_BATTERYCALIBRATION, getValS, .action = calibrate},
};

static Menu batteryMenu = {.title = "Battery",
                           .items = batMenuItems,
                           .num_items = ARRAY_SIZE(batMenuItems)};

// Main menu
static const MenuItem menuItems[] = {
    {"Scan", .submenu = &scanMenu},
    {"Radio", .submenu = &radioMenu},
    {"Equalizer", .submenu = &eqMenu},
    {"Display", .submenu = &displayMenu},
    {"Battery", .submenu = &batteryMenu},
    {"Beep", SETTING_BEEP, getValS, updateValS},
    {"Invert buttons", SETTING_INVERT_BUTTONS, getValS, updateValS},
    {"Main app", SETTING_MAINAPP, getValS, updateValS},
    {"Lock PTT", SETTING_PTT_LOCK, getValS, updateValS},
};

static Menu settingsMenu = {
    .title = "Settings",
    .items = menuItems,
    .num_items = ARRAY_SIZE(menuItems),
};

void SETTINGS_init(void) { MENU_Init(&settingsMenu); }

void SETTINGS_deinit(void) {}

bool SETTINGS_key(KEY_Code_t key, Key_State_t state) {
  return MENU_HandleInput(key, state);
}

void SETTINGS_render(void) { MENU_Render(); }
