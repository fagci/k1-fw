#include "system.h"
#include "apps/apps.h"
#include "apps/messenger.h"
#include "board.h"
#include "dcs.h"
#include "driver/backlight.h"
#include "driver/battery.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/keyboard.h"
#include "driver/lfs.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/CMSIS/Device/PY32F071/Include/py32f071xB.h"
#include "external/littlefs/lfs.h"
#include "external/printf/printf.h"
#include "helper/audio_rec.h"
#include "helper/bands.h"
#include "helper/fsk2.h"
#include "helper/keydispatch.h"
#include "helper/keymap.h"
#include "helper/lootlist.h"
#include "helper/measurements.h"
#include "helper/menu.h"
#include "helper/regs-menu.h"
#include "helper/scan.h"
#include "helper/screenshot.h"
#include "helper/storage.h"
#include "helper/vfomenu.h"
#include "inc/channel.h"
#include "misc.h"
#include "settings.h"
#include "ui/chlist.h"
#include "ui/finput.h"
#include "ui/graphics.h"
#include "ui/keymap.h"
#include "ui/lootlist.h"
#include "ui/spectrum.h"
#include "ui/statusline.h"
#include "ui/textinput.h"
#include "ui/toast.h"
#include <string.h>

static uint32_t secondTimer;
static uint32_t toastTimer;
static uint32_t backlightTimer;
static uint32_t appsKeyboardTimer;
static uint32_t intPollTimer;
static uint32_t statusLineTimer;

static void appRender(void) {
  // Подавляем все обновления дисплея (FC режим при открытом шумодаве)
  if (gSuppressDisplayUpdates) {
    return;
  }

  if (!gRedrawScreen || Now() - gLastRender < 32) {
    return;
  }

  gRedrawScreen = false;
  UI_ClearScreen();
  APPS_render();

  if (gFInputActive) {
    FINPUT_render();
  }
  if (gTextInputActive) {
    TEXTINPUT_render();
  }
  if (gLootlistActive) {
    LOOTLIST_render();
  }
  if (gChlistActive) {
    CHLIST_render();
  }
  if (gKeymapActive) {
    KEYMAP_Render();
  }

  STATUSLINE_render();
  TOAST_Render();

  gLastRender = Now();
  ST7565_Blit();
}

static void showMsg(const char *msg) {
  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, msg);
  ST7565_Blit();
}

static void resetFull(void) {
  showMsg("0xFFing...");
  PY25Q16_FullErase();
  showMsg("0xFFed!");
  for (;;) {
  }
}

static void reset(void) {
  showMsg("Formatting...");
  lfs_format(&gLfs, &gStorage.config);
  lfs_mount(&gLfs, &gStorage.config);

  showMsg("Release key 0!");
  keyboard_tick_1ms();
  while (keyboard_is_pressed(KEY_0)) {
    SYSTICK_DelayMs(1);
    keyboard_tick_1ms();
  }
  NVIC_SystemReset();
}

static void loadSettingsOrReset(void) {
  bool recreate = false;
  if (!lfs_file_exists("Settings.set")) {
    recreate = true;
  } else {
    // Проверка размера файла — если структура расширилась, пересоздаём
    struct lfs_info info;
    if (lfs_stat(&gLfs, "Settings.set", &info) == 0 &&
        info.size != sizeof(Settings)) {
      recreate = true;
    }
  }

  if (recreate) {
    STORAGE_INIT("Settings.set", Settings, 1);
    STORAGE_SAVE("Settings.set", 0, &gSettings);
  }
  STORAGE_LOAD("Settings.set", 0, &gSettings);

  // Apply global EQ settings to BK4819
  BK4819_SetAFResponse(false, false, gSettings.af_rx_300 - 4);
  BK4819_SetAFResponse(false, true, gSettings.af_rx_3k - 4);
  BK4819_SetAFResponse(true, false, gSettings.af_tx_300 - 4);
  BK4819_SetAFResponse(true, true, gSettings.af_tx_3k - 4);

  if (!lfs_file_exists("Bands.bnd")) {
    STORAGE_INIT("Bands.bnd", Band, MAX_BANDS);
  }
}


static char dtmfBuf[16] = "\0";
static uint8_t dtmfIdx = 0;
static uint32_t lastDtmf;

static bool checkInt(void) {
  if (!(BK4819_ReadRegister(0x0C) & 1)) {
    return false;
  }

  BK4819_WriteRegister(0x02, 0x0000);
  uint16_t int_bits = BK4819_ReadRegister(0x02);

  SCAN_HandleInterrupt(int_bits);

  if (int_bits & BK4819_REG_02_MASK_DTMF_5TONE_FOUND) {
    const char c = DTMF_GetCharacter(BK4819_GetDTMF_5TONE_Code());
    if (dtmfIdx < ARRAY_SIZE(dtmfBuf) - 1) {
      dtmfBuf[dtmfIdx++] = c;
      dtmfBuf[dtmfIdx] = '\0';
      lastDtmf = Now();
    }
    LogC(LOG_C_GREEN, "DTMF %c", c);
  }

  if (RF_FskReceive(int_bits)) {
    TOAST_Push("FSK: %04X %04X %04X %04x", FSK_RXDATA[0], FSK_RXDATA[1],
               FSK_RXDATA[2], FSK_RXDATA[3]);
    gHasUnreadMessages = true;
    MESSENGER_update();
  }

  return true;
}

void SYS_Main(void) {
  LogC(LOG_C_BRIGHT_WHITE, "Keyboard init");
  keyboard_init(KEYDISPATCH_onKey);
  keyboard_tick_1ms();

  if (keyboard_is_pressed(KEY_EXIT)) {
    reset();
  } else if (keyboard_is_pressed(KEY_0)) {
    resetFull();
  } else {
    loadSettingsOrReset();
    BATTERY_UpdateBatteryInfo();
    STATUSLINE_render();
    ST7565_Blit();
    LogC(LOG_C_BRIGHT_WHITE, "Run: %s", apps[gSettings.mainApp].name);
    APPS_run(gSettings.mainApp);
  }

  BACKLIGHT_TurnOn();
  LogC(LOG_C_BRIGHT_WHITE, "System initialized");

  for (;;) {
    uint32_t now = Now(); // Read once per loop — fewer TIM2 accesses

    SETTINGS_UpdateSave();
    // BK4819 IRQ polling: чтение REG_0C — SPI-транзакция, раньше шла каждую мс
    // впустую. 3 мс не влияют на DTMF/FSK/STE-tail (события идут медленнее).
    // Во время активного свипа (TUNING/CHECKING) аудио всё равно не звучит,
    // а сам периодический опрос — источник наводки, дающей "гребёнку" на
    // графике сканера (см. SCAN_IsSweeping) — поэтому пропускаем его и тут,
    // как уже сделано для Analyser.
    if ((gCurrentApp != APP_ANALYSER) && !SCAN_IsSweeping() &&
        now - intPollTimer >= 3) {
      checkInt();
      intPollTimer = now;
    }
    SCAN_Check();

    if (dtmfIdx > 0 && now - lastDtmf > 400) {
      TOAST_Push("DTMF: %s", dtmfBuf);
      dtmfIdx = 0;
    }

    if (gFInputActive) {
      FINPUT_update();
    }
    if (gTextInputActive) {
      TEXTINPUT_update();
    }
    /* if (gLootlistActive) {
      LOOTLIST_update();
    } */

    APPS_update();

    if (now - toastTimer >= 40) {
      TOAST_Update();
      toastTimer = now;
    }
    if (now - appsKeyboardTimer >= 5) {
      keyboard_tick_1ms();
      appsKeyboardTimer = now;
    }
    if (now - backlightTimer >= 500) {
      BACKLIGHT_UpdateTimer();
      backlightTimer = now;
    }
    if (now - secondTimer >= 1000) {
      BATTERY_UpdateBatteryInfo();
      secondTimer = now;
    }

    if (now - statusLineTimer >= 50) {
      STATUSLINE_update();
      statusLineTimer = now;
    }

    // Watchdog-redraw: страхует случай, когда dirty-флаг не был поднят.
    // STATUSLINE_update (раз в сек) сам ставит gRedrawScreen при изменениях,
    // поэтому здесь достаточно редкого тика. Во время активного свипа этот
    // тик — единственный периодический триггер редрава (HandleStateTuning
    // сам gRedrawScreen не трогает), а appRender() сразу за ним — это полный
    // UI_ClearScreen+APPS_render()+SPI-flush на LCD в следующей же итерации
    // после замера. Именно это давало периодическую "гребёнку" в спектре
    // (checkInt тут был ни при чём) — пропускаем watchdog-тик, пока идёт
    // TUNING/CHECKING; конец свипа и реальные кандидаты форсируют редра сами.
    if (!SCAN_IsSweeping() && now - gLastRender >= 1000) {
      gRedrawScreen = true;
    }

    appRender();

    __WFI();
  }
}
