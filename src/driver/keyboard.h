#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

// Коды кнопок
typedef enum {
  KEY_NONE = 0,
  KEY_MENU,
  KEY_UP,
  KEY_DOWN,
  KEY_EXIT,
  KEY_0,
  KEY_1,
  KEY_2,
  KEY_3,
  KEY_4,
  KEY_5,
  KEY_6,
  KEY_7,
  KEY_8,
  KEY_9,
  KEY_STAR,
  KEY_F,
  KEY_SIDE1,
  KEY_SIDE2,
  KEY_PTT,
  KEY_COUNT
} key_code_t;

// События кнопок
typedef enum {
  KEY_EVENT_PRESS,   // Кнопка нажата (после debounce)
  KEY_EVENT_RELEASE, // Кнопка отпущена
  KEY_EVENT_HOLD, // Кнопка удерживается (первое срабатывание)
  KEY_EVENT_REPEAT // Повторное срабатывание при удержании
} key_event_t;

// Конфигурация таймингов
typedef struct {
  uint16_t debounce_ms; // Время антидребезга (по умолчанию 20ms)
  uint16_t hold_delay_ms; // Задержка до первого HOLD (по умолчанию 500ms)
  uint16_t repeat_delay_ms; // Интервал повтора (по умолчанию 100ms)
  bool repeat_enabled; // Включить автоповтор
} key_timing_config_t;

// Callback для обработки событий
typedef void (*key_event_callback_t)(key_code_t key, key_event_t event);

// Инициализация клавиатуры
void keyboard_init(key_event_callback_t callback);

// Установить конфигурацию таймингов
void keyboard_set_timing(const key_timing_config_t *config);

// Получить конфигурацию по умолчанию
key_timing_config_t keyboard_get_default_timing(void);

// Вызывать каждые 1ms из таймера
void keyboard_tick_1ms(void);

// Получить текущее состояние кнопки (нажата/не нажата)
bool keyboard_is_pressed(key_code_t key);

#endif // KEYBOARD_H
