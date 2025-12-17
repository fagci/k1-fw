#include "board.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/bk4829.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "ui/graphics.h"

static const uint32_t fStart = 17200000;
static const uint32_t fEnd = fStart + 2500 * LCD_WIDTH;
static uint32_t f;

static uint32_t delay = 1200;

uint16_t rssiHistory[LCD_WIDTH];
bool redraw = true;
uint32_t lastRedrawtime;

typedef struct {
  KEY_Code_t current;
  KEY_Code_t prev;
  uint8_t counter;
} KeyboardState;

KeyboardState kbd = {KEY_INVALID, KEY_INVALID, 0};

static bool HandleUserInput() {
  kbd.prev = kbd.current;
  kbd.current = KEYBOARD_Poll();

  if (kbd.current != KEY_INVALID && kbd.current == kbd.prev) {
    if (kbd.counter < 16)
      kbd.counter++;
    else
      kbd.counter -= 3;
    SYSTICK_DelayMs(20);
  } else {
    kbd.counter = 0;
  }

  if (kbd.counter == 3 || kbd.counter == 16) {
    switch (kbd.current) {
    case KEY_1:
    case KEY_7:
      delay += kbd.current == KEY_1 ? 100 : -100;
      return true;
    default:
      break;
    }
  }

  return false;
}

uint16_t measure(uint32_t freq) {
  BK4819_TuneTo(freq, true);
  SYSTICK_DelayUs(delay);
  return BK4819_GetRSSI();
}

void tick() {
  uint16_t rssi = measure(f);
  uint8_t x = ConvertDomain(f, fStart, fEnd, 0, LCD_WIDTH - 1);
  // printf("f=%u x=%u flt=%u rssi=%u\n", f, x, b, rssi);

  if (rssi > rssiHistory[x]) {
    rssiHistory[x] = rssi;
  }

  if (f >= fEnd) {
    f = fStart;
    UI_ClearScreen();

    uint16_t mi = 512;
    uint16_t ma = 0;
    for (uint8_t i = 0; i < LCD_WIDTH; ++i) {
      uint16_t r = rssiHistory[i];
      if (r > ma)
        ma = r;
      if (r < mi && r > 0)
        mi = r;
    }

    uint16_t realMax = ma;

    uint16_t scale = ma - mi;
    if (scale < 40) {
      ma = mi + 40;
    }

    for (uint8_t i = 0; i < LCD_WIDTH; ++i) {
      uint16_t r = rssiHistory[i];
      if (r > 0) {
        uint8_t v = ConvertDomain(r, mi, ma, 1, LCD_HEIGHT - 16);
        DrawVLine(i, LCD_HEIGHT - v, v, C_FILL);
      }
    }
    for (uint8_t i = 0; i < LCD_WIDTH; ++i) {
      rssiHistory[i] = 0;
    }

    PrintSmall(0, 14, "%uus", delay);
    PrintSmall(0, 14 + 8, "Max: %u (%u)", realMax, ma);
    PrintSmall(0, 20 + 8, "Min: %u", mi);
    redraw = true;
  } else {
    f += 2500;
  }

  if (redraw && Now() - lastRedrawtime >= 40) {
    redraw = false;
    lastRedrawtime = Now();
    ST7565_BlitFullScreen();
  }
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();
  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  f = fStart;
  BK4819_TuneTo(f, true);
  BK4819_SelectFilter(f);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 0);

  BACKLIGHT_TurnOn();

  for (;;) {
    if (HandleUserInput()) {
      redraw = true;
    }
    tick();
  }
}
