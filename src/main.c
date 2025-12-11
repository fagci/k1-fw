#include "board.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/bk4829.h"
#include "driver/crc.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "ui/graphics.h"
#include <stdbool.h>

static uint32_t fInitial = 17230000;

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  BK4819_TuneTo(fInitial, true);
  BK4819_SelectFilter(fInitial);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 0);

  AUDIO_AudioPathOn();

  BACKLIGHT_TurnOn();

  bool b = false;
  for (;;) {
    bool b = !b;
    BK4819_SelectFilterEx(b ? FILTER_UHF : FILTER_VHF);
    printf("RSSI=%u\n", BK4819_GetRSSI());
    BOARD_FlashlightToggle();
    UI_ClearScreen();
    PrintBigDigitsEx(LCD_WIDTH - 1, 32, POS_R, C_FILL, "%u",
                     BK4819_GetFrequency());
    PrintMedium(0, 40, "RSSI: %u", BK4819_GetRSSI());
    PrintMedium(0, 48, "NOW: %u", Now());
    PrintMedium(0, 56, "Key: %u", KEYBOARD_Poll());
    ST7565_BlitFullScreen();
    BK4819_ToggleGpioOut(BK4819_GREEN, b);
    SYSTICK_DelayMs(500);
  }
}
