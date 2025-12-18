#include "spectrum.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include <string.h>

#define SPECTRUM_HEIGHT 48 // Высота графика
#define SPECTRUM_TOP 8     // Отступ сверху

static spectrum_config_t g_config;
static measure_callback_t g_measure_func;

// Буферы данных
static int16_t g_trace_buffer[LCD_WIDTH]; // Текущая развёртка
static int16_t g_max_hold_buffer[LCD_WIDTH]; // Буфер max hold
static uint32_t g_avg_buffer[LCD_WIDTH]; // Буфер для усреднения
static uint8_t g_avg_count = 0;

// Состояние развёртки
static uint16_t g_sweep_point = 0;
static bool g_sweep_complete = false;

// ============================================================================
// Инициализация
// ============================================================================

void spectrum_init(measure_callback_t measure_func) {
  g_measure_func = measure_func;
  g_config = spectrum_get_default_config();

  // Очистка буферов
  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    g_trace_buffer[i] = -1000; // -100 дБм
    g_max_hold_buffer[i] = -1000;
    g_avg_buffer[i] = 0;
  }
  g_avg_count = 0;
  g_sweep_point = 0;
  g_sweep_complete = false;
}

spectrum_config_t spectrum_get_default_config(void) {
  spectrum_config_t cfg = {.freq_start = 1720000, // 17.2 МГц в 10 Гц
                           .freq_end = 1720000 + 250 * LCD_WIDTH, // +320 кГц
                           .freq_center = 1720000 + 250 * LCD_WIDTH / 2,
                           .freq_span = 250 * LCD_WIDTH,
                           .sweep_delay_us = 1200,
                           .ref_level = -800, // 0 дБм
                           .scale_div = 10,   // 10 дБ/деление
                           .display_mode = DISPLAY_MODE_NORMAL,
                           .auto_scale = true,
                           .grid_enabled = true,
                           .active_marker = 0};

  // Инициализация маркеров
  for (uint8_t i = 0; i < MAX_MARKERS; i++) {
    cfg.markers[i].enabled = false;
    cfg.markers[i].x = LCD_WIDTH / 2;
  }
  cfg.markers[0].enabled = true; // Маркер 1 включен по умолчанию

  return cfg;
}

void spectrum_set_config(const spectrum_config_t *config) {
  g_config = *config;
}

spectrum_config_t *spectrum_get_config(void) { return &g_config; }

// ============================================================================
// Управление частотой
// ============================================================================

void spectrum_set_center_span(uint32_t center, uint32_t span) {
  g_config.freq_center = center;
  g_config.freq_span = span;
  g_config.freq_start = center - span / 2;
  g_config.freq_end = center + span / 2;
  spectrum_reset_sweep();
}

void spectrum_set_start_stop(uint32_t start, uint32_t stop) {
  g_config.freq_start = start;
  g_config.freq_end = stop;
  g_config.freq_center = (start + stop) / 2;
  g_config.freq_span = stop - start;
  spectrum_reset_sweep();
}

void spectrum_zoom_in(void) {
  uint32_t new_span = g_config.freq_span / 2;
  if (new_span < 1000)
    new_span = 1000; // Минимум 10 кГц
  spectrum_set_center_span(g_config.freq_center, new_span);
}

void spectrum_zoom_out(void) {
  uint32_t new_span = g_config.freq_span * 2;
  if (new_span > 10000000)
    new_span = 10000000; // Максимум 100 МГц
  spectrum_set_center_span(g_config.freq_center, new_span);
}

// ============================================================================
// Управление уровнем
// ============================================================================

void spectrum_set_ref_level(int16_t level_dbm_x10) {
  g_config.ref_level = level_dbm_x10;
}

void spectrum_adjust_ref_level(int16_t delta_db) {
  g_config.ref_level += delta_db * 10;
}

void spectrum_auto_scale(void) {
  // Найти максимум и минимум
  int16_t max_val = -1000;
  int16_t min_val = 0;

  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    if (g_trace_buffer[i] > max_val) {
      max_val = g_trace_buffer[i];
    }
    if (g_trace_buffer[i] < min_val && g_trace_buffer[i] > -999) {
      min_val = g_trace_buffer[i];
    }
  }

  // Установить ref level чуть выше максимума
  g_config.ref_level = max_val + 50; // +5 дБ запаса
}

// ============================================================================
// Развёртка
// ============================================================================

void spectrum_tick(void) {
  if (!g_measure_func || g_sweep_complete)
    return;

  if (g_sweep_point < LCD_WIDTH) {
    // Вычислить частоту для текущей точки
    uint32_t freq =
        g_config.freq_start + (g_config.freq_span * g_sweep_point) / LCD_WIDTH;

    // Измерить уровень
    int16_t level = g_measure_func(freq);
    g_trace_buffer[g_sweep_point] = level;

    // Max hold
    if (g_config.display_mode == DISPLAY_MODE_MAX_HOLD) {
      if (level > g_max_hold_buffer[g_sweep_point]) {
        g_max_hold_buffer[g_sweep_point] = level;
      }
    }

    // Averaging
    if (g_config.display_mode == DISPLAY_MODE_AVG) {
      g_avg_buffer[g_sweep_point] += level;
    }

    g_sweep_point++;
  } else {
    // Развёртка завершена
    g_sweep_complete = true;

    // Усреднение
    if (g_config.display_mode == DISPLAY_MODE_AVG) {
      g_avg_count++;
      if (g_avg_count > 10) { // Сброс после 10 развёрток
        for (uint16_t i = 0; i < LCD_WIDTH; i++) {
          g_avg_buffer[i] = 0;
        }
        g_avg_count = 0;
      }
    }

    // Обновление маркеров
    for (uint8_t m = 0; m < MAX_MARKERS; m++) {
      if (g_config.markers[m].enabled) {
        uint16_t x = g_config.markers[m].x;
        if (x < LCD_WIDTH) {
          g_config.markers[m].freq =
              g_config.freq_start + (g_config.freq_span * x) / LCD_WIDTH;
          g_config.markers[m].level = g_trace_buffer[x];
        }
      }
    }
  }
}

bool spectrum_is_complete(void) { return g_sweep_complete; }

void spectrum_reset_sweep(void) {
  g_sweep_point = 0;
  g_sweep_complete = false;
}

// ============================================================================
// Маркеры
// ============================================================================

void spectrum_marker_enable(uint8_t marker_num, bool enable) {
  if (marker_num < MAX_MARKERS) {
    g_config.markers[marker_num].enabled = enable;
  }
}

void spectrum_marker_to_center(uint8_t marker_num) {
  if (marker_num < MAX_MARKERS && g_config.markers[marker_num].enabled) {
    uint32_t marker_freq = g_config.markers[marker_num].freq;
    spectrum_set_center_span(marker_freq, g_config.freq_span);
  }
}

void spectrum_marker_to_peak(uint8_t marker_num) {
  if (marker_num >= MAX_MARKERS)
    return;

  // Найти пик
  int16_t max_level = -1000;
  uint16_t max_x = 0;

  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    if (g_trace_buffer[i] > max_level) {
      max_level = g_trace_buffer[i];
      max_x = i;
    }
  }

  g_config.markers[marker_num].x = max_x;
  g_config.markers[marker_num].enabled = true;
}

void spectrum_marker_move(uint8_t marker_num, int16_t delta_x) {
  if (marker_num < MAX_MARKERS && g_config.markers[marker_num].enabled) {
    int16_t new_x = (int16_t)g_config.markers[marker_num].x + delta_x;
    if (new_x < 0)
      new_x = 0;
    if (new_x >= LCD_WIDTH)
      new_x = LCD_WIDTH - 1;
    g_config.markers[marker_num].x = new_x;
  }
}

void spectrum_marker_delta(uint8_t marker_ref, uint8_t marker_delta) {
  // Показать разницу между двумя маркерами
  if (marker_ref >= MAX_MARKERS || marker_delta >= MAX_MARKERS)
    return;
  if (!g_config.markers[marker_ref].enabled ||
      !g_config.markers[marker_delta].enabled)
    return;

  // Эта функция для UI - можно использовать в draw
}

// ============================================================================
// Режимы
// ============================================================================

void spectrum_set_mode(display_mode_t mode) {
  g_config.display_mode = mode;
  if (mode != DISPLAY_MODE_AVG) {
    g_avg_count = 0;
    for (uint16_t i = 0; i < LCD_WIDTH; i++) {
      g_avg_buffer[i] = 0;
    }
  }
}

void spectrum_clear_max_hold(void) {
  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    g_max_hold_buffer[i] = -1000;
  }
}

// ============================================================================
// Отрисовка
// ============================================================================

static uint8_t level_to_y(int16_t level_dbm_x10) {
  // Преобразование уровня в координату Y
  int16_t range_top = g_config.ref_level;
  int16_t range_bottom = range_top - (g_config.scale_div * 10 *
                                      (SPECTRUM_HEIGHT / g_config.scale_div));

  if (level_dbm_x10 >= range_top)
    return SPECTRUM_TOP;
  if (level_dbm_x10 <= range_bottom)
    return SPECTRUM_TOP + SPECTRUM_HEIGHT;

  uint8_t y = ConvertDomain(level_dbm_x10, range_bottom, range_top,
                            SPECTRUM_TOP + SPECTRUM_HEIGHT, SPECTRUM_TOP);
  return y;
}

static void draw_grid(void) {
  if (!g_config.grid_enabled)
    return;

  // Горизонтальные линии сетки (каждые 10 пикселей ~ 10 дБ)
  for (uint8_t i = 0; i <= 4; i++) {
    uint8_t y = SPECTRUM_TOP + i * 12;
    if (y > SPECTRUM_TOP + SPECTRUM_HEIGHT)
      break;
    for (uint8_t x = 0; x < LCD_WIDTH; x += 4) {
      PutPixel(x, y, C_FILL);
    }
  }

  // Вертикальные линии сетки (через каждые 21 пиксель)
  for (uint8_t x = 0; x < LCD_WIDTH; x += 21) {
    for (uint8_t y = SPECTRUM_TOP; y < SPECTRUM_TOP + SPECTRUM_HEIGHT; y += 4) {
      PutPixel(x, y, C_FILL);
    }
  }

  // Центральная линия (жирная)
  uint8_t center_x = LCD_WIDTH / 2;
  for (uint8_t y = SPECTRUM_TOP; y < SPECTRUM_TOP + SPECTRUM_HEIGHT; y += 2) {
    PutPixel(center_x, y, C_FILL);
  }
}

static void draw_trace(void) {
  int16_t *buffer = g_trace_buffer;

  // Выбрать буфер в зависимости от режима
  if (g_config.display_mode == DISPLAY_MODE_MAX_HOLD) {
    buffer = g_max_hold_buffer;
  } else if (g_config.display_mode == DISPLAY_MODE_AVG && g_avg_count > 0) {
    // Использовать усреднённые данные
    static int16_t avg_trace[LCD_WIDTH];
    for (uint16_t i = 0; i < LCD_WIDTH; i++) {
      avg_trace[i] = g_avg_buffer[i] / g_avg_count;
    }
    buffer = avg_trace;
  }

  // Рисовать линию
  uint8_t prev_y = level_to_y(buffer[0]);
  for (uint16_t x = 1; x < LCD_WIDTH; x++) {
    uint8_t y = level_to_y(buffer[x]);

    // Рисовать линию от предыдущей точки
    if (prev_y < y) {
      DrawVLine(x, prev_y, y - prev_y + 1, C_FILL);
    } else if (prev_y > y) {
      DrawVLine(x, y, prev_y - y + 1, C_FILL);
    } else {
      PutPixel(x, y, C_FILL);
    }

    prev_y = y;
  }
}

static void draw_markers(void) {
  for (uint8_t m = 0; m < MAX_MARKERS; m++) {
    if (!g_config.markers[m].enabled)
      continue;

    marker_t *marker = &g_config.markers[m];
    uint8_t x = marker->x;

    // Вертикальная линия маркера
    for (uint8_t y = SPECTRUM_TOP; y < SPECTRUM_TOP + SPECTRUM_HEIGHT; y += 2) {
      PutPixel(x, y, C_FILL);
    }

    // Треугольник наверху
    PutPixel(x, SPECTRUM_TOP - 2, C_FILL);
    DrawHLine(x - 1, SPECTRUM_TOP - 1, 3, C_FILL);

    // Номер маркера
    char buf[4];
    sprintf(buf, "%d", m + 1);
    PrintSmall(x - 2, SPECTRUM_TOP - 7 + 5, buf);
  }
}

static void draw_info(void) {
  char buf[32];

  // Верхняя строка - Start частота слева
  uint32_t f_mhz = g_config.freq_start / 100000;
  uint32_t f_khz = (g_config.freq_start % 100000) / 100;
  sprintf(buf, "%lu.%03lu", f_mhz, f_khz);
  PrintSmall(0, 5, buf);

  // End частота справа
  f_mhz = g_config.freq_end / 100000;
  f_khz = (g_config.freq_end % 100000) / 100;
  sprintf(buf, "%lu.%03lu", f_mhz, f_khz);
  uint8_t text_width = strlen(buf) * 4;
  PrintSmall(LCD_WIDTH - text_width, 5, buf);

  // Центральная частота
  f_mhz = g_config.freq_center / 100000;
  f_khz = (g_config.freq_center % 100000) / 100;
  sprintf(buf, "%lu.%03lu", f_mhz, f_khz);
  text_width = strlen(buf) * 4;
  PrintSmall((LCD_WIDTH - text_width) / 2, 5, buf);

  // Нижняя строка - маркеры и уровень
  uint8_t y = SPECTRUM_TOP + SPECTRUM_HEIGHT + 2 + 5;

  // Активный маркер слева
  if (g_config.markers[g_config.active_marker].enabled) {
    marker_t *m = &g_config.markers[g_config.active_marker];
    f_mhz = m->freq / 100000;
    f_khz = (m->freq % 100000) / 100;
    sprintf(buf, "M%d:%lu.%03lu", g_config.active_marker + 1, f_mhz, f_khz);
    PrintSmall(0, y, buf);

    // Уровень маркера
    y += 8;
    sprintf(buf, "%d.%ddBm", m->level / 10, abs(m->level % 10));
    PrintSmall(0, y, buf);
  }

  // Ref level справа сверху
  sprintf(buf, "R:%ddB", g_config.ref_level / 10);
  text_width = strlen(buf) * 4;
  PrintSmall(LCD_WIDTH - text_width, SPECTRUM_TOP + SPECTRUM_HEIGHT + 2 + 5,
             buf);

  // Span справа снизу
  uint32_t span_khz = g_config.freq_span / 100;
  sprintf(buf, "S:%lukHz", span_khz);
  text_width = strlen(buf) * 4;
  PrintSmall(LCD_WIDTH - text_width, SPECTRUM_TOP + SPECTRUM_HEIGHT + 2 + 5 + 8,
             buf);
}

void spectrum_draw(void) {
  UI_ClearStatus();
  UI_ClearScreen();

  draw_grid();
  draw_trace();
  draw_markers();
  draw_info();
}
