#include "board.h"
#include "driver/backlight.h"
#include "driver/bk4829.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "spectrum.h"
#include "ui/graphics.h"
#include <stdbool.h>

const char *BW_NAMES[] = {
    [BK4819_FILTER_BW_6k] = "U 6K",   //
    [BK4819_FILTER_BW_7k] = "U 7K",   //
    [BK4819_FILTER_BW_9k] = "N 9k",   //
    [BK4819_FILTER_BW_10k] = "N 10k", //
    [BK4819_FILTER_BW_12k] = "W 12k", //
    [BK4819_FILTER_BW_14k] = "W 14k", //
    [BK4819_FILTER_BW_17k] = "W 17k", //
    [BK4819_FILTER_BW_20k] = "W 20k", //
    [BK4819_FILTER_BW_23k] = "W 23k", //
    [BK4819_FILTER_BW_26k] = "W 26k", //
};

static bool precise = true;
static uint32_t delay = 1200;
static uint8_t gain = 0;
static BK4819_FilterBandwidth_t bw = BK4819_FILTER_BW_10k;
static Band band = {
    .start = 172 * MHZ,
    .end = 172 * MHZ + 25 * KHZ * 128,
    .step = 25 * KHZ,
};
static Measurement msm;

static uint32_t testF = 17230000;

void measure() {
  BK4819_SelectFilter(msm.f);
  BK4819_TuneTo(msm.f, precise);
  SYSTICK_DelayUs(delay);
  msm.rssi = BK4819_GetRSSI();
  SP_AddPoint(&msm);
}

void onKey(key_code_t key, key_event_t event) {
  if (event != KEY_EVENT_PRESS && event != KEY_EVENT_REPEAT) {
    return;
  }

  switch (key) {
  case KEY_1:
  case KEY_7:
    delay = AdjustU(delay, 100, 10000, key == KEY_1 ? 100 : -100);
    break;
  case KEY_2:
  case KEY_8:
    bw = IncDecU(bw, BK4819_FILTER_BW_6k, BK4819_FILTER_BW_26k, key == KEY_2);
    break;
  case KEY_3:
  case KEY_9:
    gain = IncDecU(gain, 0, 32, key == KEY_3);
    BK4819_SetAGC(true, gain);
    break;
  case KEY_0:
    precise = !precise;
    break;
  }
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, gain);

  BACKLIGHT_TurnOn();
  keyboard_init(onKey);

  SP_Init(band);
  msm.f = band.start;

  for (;;) {
    keyboard_tick_1ms();

    measure();

    if (msm.f >= band.end) {
      UI_ClearStatus();
      UI_ClearScreen();
      SP_Draw();

      PrintSmall(0, 8 + 6 * 1, "%ums %u", delay, precise);
      PrintSmall(0, 8 + 6 * 2, "%s", BW_NAMES[bw]);
      PrintSmall(0, 8 + 6 * 3, "%u", BK4819_GetAttenuation());

      PrintSmall(0, LCD_HEIGHT - 1, "%u.%05u", band.start / MHZ,
                 band.start % MHZ);
      PrintSmallEx(LCD_WIDTH, LCD_HEIGHT - 1, POS_R, C_FILL, "%u.%05u",
                   band.end / MHZ, band.end % MHZ);

      BK4819_TuneTo(testF, true);
      SYSTICK_DelayMs(50);

      PrintSmallEx(LCD_WIDTH, 8 + 6 * 1, POS_R, C_FILL, "R %3u N %3u G %3u",
                   BK4819_GetRSSI(), BK4819_GetNoise(), BK4819_GetGlitch());
      PrintSmallEx(LCD_WIDTH, 8 + 6 * 2, POS_R, C_FILL, "POW L %u U %u",
                   BK4819_GetLowerChannelRelativePower(),
                   BK4819_GetUpperChannelRelativePower());
      PrintSmallEx(LCD_WIDTH, 8 + 6 * 3, POS_R, C_FILL, "%3ddB",
                   Rssi2DBm(BK4819_GetRSSI()));

      ST7565_BlitFullScreen();
      SP_Init(band);
      msm.f = band.start;
    } else {
      msm.f += band.step;
    }
  }
}
