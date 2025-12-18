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

static uint32_t g_sweep_delay = 1200;

int16_t measure_level(uint32_t freq) {
  BK4819_TuneTo(freq, true);
  SYSTICK_DelayUs(g_sweep_delay);
  uint16_t rssi = BK4819_GetRSSI();
  // printf("msm: %u\n", rssi);

  // Преобразовать RSSI в дБм * 10
  // Калибровка зависит от вашего приёмника
  int16_t dbm_x10 = Rssi2DBm(rssi) * 10; // Пример
  return dbm_x10;
}

void onKey(key_code_t key, key_event_t event) {
  if (event != KEY_EVENT_PRESS && event != KEY_EVENT_REPEAT)
    return;

  spectrum_config_t *cfg = spectrum_get_config();

  switch (key) {
  case KEY_UP:
    spectrum_adjust_ref_level(1); // +1 дБ
    break;

  case KEY_DOWN:
    spectrum_adjust_ref_level(-1); // -1 дБ
    break;

  case KEY_1: // Zoom In
    spectrum_zoom_in();
    break;

  case KEY_2: // Zoom Out
    spectrum_zoom_out();
    break;

  case KEY_3: // Marker влево
    spectrum_marker_move(cfg->active_marker, -1);
    break;

  case KEY_6: // Marker вправо
    spectrum_marker_move(cfg->active_marker, 1);
    break;

  case KEY_5: // Marker -> Center
    spectrum_marker_to_center(cfg->active_marker);
    break;

  case KEY_STAR: // Marker -> Peak
    spectrum_marker_to_peak(cfg->active_marker);
    break;

  case KEY_0: // Переключение маркера
    cfg->active_marker = (cfg->active_marker + 1) % MAX_MARKERS;
    spectrum_marker_enable(cfg->active_marker, true);
    break;

  case KEY_7: // Увеличить задержку
    g_sweep_delay += 100;
    cfg->sweep_delay_us = g_sweep_delay;
    break;

  case KEY_4: // Уменьшить задержку
    if (g_sweep_delay > 100) {
      g_sweep_delay -= 100;
      cfg->sweep_delay_us = g_sweep_delay;
    }
    break;

  case KEY_8: // Переключение режима
    display_mode_t mode = cfg->display_mode;
    mode = (mode + 1) % 3;
    spectrum_set_mode(mode);
    if (mode == DISPLAY_MODE_MAX_HOLD)
      spectrum_clear_max_hold();
    break;

  case KEY_9: // Auto Scale
    spectrum_auto_scale();
    break;

  case KEY_F: // Toggle Grid
    cfg->grid_enabled = !cfg->grid_enabled;
    break;
  }
}

int main(void) {
  // Инициализация как у вас
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();
  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  BK4819_SelectFilter(17200000);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 0);

  BACKLIGHT_TurnOn();
  keyboard_init(onKey);

  // Инициализация анализатора спектра
  spectrum_init(measure_level);

  // Настройка диапазона
  spectrum_set_start_stop(17200000, 17200000 + 2500 * LCD_WIDTH);

  uint32_t last_sweep = 0;

  for (;;) {
    keyboard_tick_1ms();
    spectrum_tick();

    if (spectrum_is_complete()) {
      spectrum_draw();
      ST7565_BlitFullScreen();
      spectrum_reset_sweep();
    }
  }
}
