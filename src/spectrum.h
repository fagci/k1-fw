#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>

// Типы режимов отображения
typedef enum {
    DISPLAY_MODE_NORMAL,      // Обычный режим
    DISPLAY_MODE_MAX_HOLD,    // Держим максимумы
    DISPLAY_MODE_AVG          // Усреднение
} display_mode_t;

// Маркеры
typedef struct {
    bool enabled;
    uint16_t x;           // Позиция на экране
    uint32_t freq;        // Частота в 10 Гц
    int16_t level;        // Уровень в дБм * 10
} marker_t;

#define MAX_MARKERS 4

// Конфигурация спектроанализатора
typedef struct {
    uint32_t freq_start;      // В 10 Гц (например 1720000 = 17.2 МГц)
    uint32_t freq_end;
    uint32_t freq_center;
    uint32_t freq_span;
    
    uint16_t sweep_delay_us;  // Задержка на точку в мкс
    
    int16_t ref_level;        // Опорный уровень дБм * 10
    uint8_t scale_div;        // дБ на деление (обычно 10)
    
    display_mode_t display_mode;
    
    bool auto_scale;
    bool grid_enabled;
    
    marker_t markers[MAX_MARKERS];
    uint8_t active_marker;
} spectrum_config_t;

// Callback для измерения уровня сигнала на частоте
// freq - частота в 10 Гц
// Возвращает уровень в дБм * 10 (например, -850 = -85.0 дБм)
typedef int16_t (*measure_callback_t)(uint32_t freq);

// Инициализация
void spectrum_init(measure_callback_t measure_func);

// Получить конфигурацию по умолчанию
spectrum_config_t spectrum_get_default_config(void);

// Установить конфигурацию
void spectrum_set_config(const spectrum_config_t *config);

// Получить текущую конфигурацию
spectrum_config_t* spectrum_get_config(void);

// Управление частотой (все частоты в 10 Гц)
void spectrum_set_center_span(uint32_t center, uint32_t span);
void spectrum_set_start_stop(uint32_t start, uint32_t stop);
void spectrum_zoom_in(void);    // Уменьшить span в 2 раза
void spectrum_zoom_out(void);   // Увеличить span в 2 раза

// Управление уровнем
void spectrum_set_ref_level(int16_t level_dbm_x10);
void spectrum_adjust_ref_level(int16_t delta_db);
void spectrum_auto_scale(void);

// Развёртка (вызывать в главном цикле)
void spectrum_tick(void);       // Измерить одну точку
bool spectrum_is_complete(void); // Развёртка завершена?
void spectrum_reset_sweep(void); // Начать новую развёртку

// Маркеры
void spectrum_marker_enable(uint8_t marker_num, bool enable);
void spectrum_marker_to_center(uint8_t marker_num);
void spectrum_marker_to_peak(uint8_t marker_num);
void spectrum_marker_move(uint8_t marker_num, int16_t delta_x);
void spectrum_marker_delta(uint8_t marker_ref, uint8_t marker_delta); // Дельта между маркерами

// Режимы
void spectrum_set_mode(display_mode_t mode);
void spectrum_clear_max_hold(void);

// Отрисовка (вызывать когда is_complete())
void spectrum_draw(void);

#endif // SPECTRUM_H
