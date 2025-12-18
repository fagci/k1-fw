
// ============================================================================
// РЕАЛИЗАЦИЯ
// ============================================================================

#include "gpio.h"
#include "keyboard.h"
#include "systick.h"

// Состояния FSM для каждой кнопки
typedef enum {
  STATE_IDLE,            // Кнопка не нажата
  STATE_DEBOUNCE,        // Дребезг при нажатии
  STATE_PRESSED,         // Кнопка стабильно нажата
  STATE_HOLD_WAIT,       // Ожидание долгого нажатия
  STATE_HOLD,            // Долгое нажатие
  STATE_REPEAT,          // Режим автоповтора
  STATE_RELEASE_DEBOUNCE // Дребезг при отпускании
} key_state_t;

// Контекст для каждой кнопки
typedef struct {
  key_state_t state;
  uint16_t counter;
  bool physical_state; // Текущее физическое состояние
} key_context_t;

// Глобальные переменные
static key_context_t g_keys[KEY_COUNT];
static key_event_callback_t g_callback;
static key_timing_config_t g_timing;

// GPIO конфигурация
#define GPIOx GPIOB
#define PIN_MASK_COLS                                                          \
  (LL_GPIO_PIN_6 | LL_GPIO_PIN_5 | LL_GPIO_PIN_4 | LL_GPIO_PIN_3)
#define PIN_COLS GPIO_MAKE_PIN(GPIOx, PIN_MASK_COLS)
#define PIN_COL(n) GPIO_MAKE_PIN(GPIOx, 1u << (6 - (n)))
#define PIN_MASK_ROWS                                                          \
  (LL_GPIO_PIN_15 | LL_GPIO_PIN_14 | LL_GPIO_PIN_13 | LL_GPIO_PIN_12)
#define PIN_MASK_ROW(n) (1u << (15 - (n)))

// Матрица клавиатуры
static const key_code_t g_keymap[5][4] = {
    {KEY_SIDE1, KEY_SIDE2, KEY_NONE, KEY_NONE},
    {KEY_MENU, KEY_1, KEY_4, KEY_7},
    {KEY_UP, KEY_2, KEY_5, KEY_8},
    {KEY_DOWN, KEY_3, KEY_6, KEY_9},
    {KEY_EXIT, KEY_STAR, KEY_0, KEY_F}};

// ============================================================================
// Низкоуровневое сканирование
// ============================================================================

static inline uint32_t read_rows(void) {
  return PIN_MASK_ROWS & LL_GPIO_ReadInputPort(GPIOx);
}

static key_code_t scan_matrix(void) {
  for (uint8_t col = 0; col < 5; col++) {
    // Установить все колонки в HIGH
    GPIO_SetOutputPin(PIN_COLS);

    // Опустить текущую колонку в LOW
    if (col > 0) {
      GPIO_ResetOutputPin(PIN_COL(col - 1));
    }

    // Антидребезг при чтении
    uint32_t stable_read = 0;
    uint8_t stable_count = 0;

    for (uint8_t i = 0; i < 8 && stable_count < 3; i++) {
      SYSTICK_DelayUs(1);
      uint32_t current_read = read_rows();

      if (current_read == stable_read) {
        stable_count++;
      } else {
        stable_read = current_read;
        stable_count = 1;
      }
    }

    // Если не удалось получить стабильное чтение - слишком много шума
    if (stable_count < 3) {
      continue;
    }

    // Проверить каждую строку
    for (uint8_t row = 0; row < 4; row++) {
      if (!(stable_read & PIN_MASK_ROW(row))) {
        return g_keymap[col][row];
      }
    }
  }

  return KEY_NONE;
}

static bool scan_ptt(void) { return GPIO_IsPttPressed(); }

// ============================================================================
// FSM обработка
// ============================================================================

static void process_key_fsm(key_code_t key, bool is_pressed) {
  key_context_t *ctx = &g_keys[key];

  switch (ctx->state) {
  case STATE_IDLE:
    if (is_pressed) {
      ctx->state = STATE_DEBOUNCE;
      ctx->counter = 0;
    }
    break;

  case STATE_DEBOUNCE:
    if (!is_pressed) {
      // Ложное срабатывание
      ctx->state = STATE_IDLE;
    } else if (++ctx->counter >= g_timing.debounce_ms) {
      // Дребезг прошёл - кнопка действительно нажата
      ctx->state = STATE_PRESSED;
      ctx->counter = 0;
      if (g_callback) {
        g_callback(key, KEY_EVENT_PRESS);
      }

      // Переход к ожиданию удержания
      if (g_timing.hold_delay_ms > 0) {
        ctx->state = STATE_HOLD_WAIT;
      }
    }
    break;

  case STATE_PRESSED:
    if (!is_pressed) {
      ctx->state = STATE_RELEASE_DEBOUNCE;
      ctx->counter = 0;
    }
    break;

  case STATE_HOLD_WAIT:
    if (!is_pressed) {
      ctx->state = STATE_RELEASE_DEBOUNCE;
      ctx->counter = 0;
    } else if (++ctx->counter >= g_timing.hold_delay_ms) {
      // Кнопка удерживается достаточно долго
      ctx->state = STATE_HOLD;
      ctx->counter = 0;
      if (g_callback) {
        g_callback(key, KEY_EVENT_HOLD);
      }

      // Если включен автоповтор
      if (g_timing.repeat_enabled) {
        ctx->state = STATE_REPEAT;
      }
    }
    break;

  case STATE_HOLD:
    if (!is_pressed) {
      ctx->state = STATE_RELEASE_DEBOUNCE;
      ctx->counter = 0;
    }
    break;

  case STATE_REPEAT:
    if (!is_pressed) {
      ctx->state = STATE_RELEASE_DEBOUNCE;
      ctx->counter = 0;
    } else if (++ctx->counter >= g_timing.repeat_delay_ms) {
      ctx->counter = 0;
      if (g_callback) {
        g_callback(key, KEY_EVENT_REPEAT);
      }
    }
    break;

  case STATE_RELEASE_DEBOUNCE:
    if (is_pressed) {
      // Кнопка снова нажата до окончания дребезга
      ctx->state = STATE_PRESSED;
    } else if (++ctx->counter >= g_timing.debounce_ms) {
      // Кнопка действительно отпущена
      ctx->state = STATE_IDLE;
      ctx->counter = 0;
      if (g_callback) {
        g_callback(key, KEY_EVENT_RELEASE);
      }
    }
    break;
  }

  ctx->physical_state = is_pressed;
}

// ============================================================================
// Публичный API
// ============================================================================

void keyboard_init(key_event_callback_t callback) {
  g_callback = callback;
  g_timing = keyboard_get_default_timing();

  // Сброс всех состояний
  for (uint8_t i = 0; i < KEY_COUNT; i++) {
    g_keys[i].state = STATE_IDLE;
    g_keys[i].counter = 0;
    g_keys[i].physical_state = false;
  }
}

key_timing_config_t keyboard_get_default_timing(void) {
  key_timing_config_t config = {.debounce_ms = 20,
                                .hold_delay_ms = 500,
                                .repeat_delay_ms = 100,
                                .repeat_enabled = true};
  return config;
}

void keyboard_set_timing(const key_timing_config_t *config) {
  g_timing = *config;
}

void keyboard_tick_1ms(void) {
  // Сканировать матрицу
  key_code_t matrix_key = scan_matrix();

  // Обработать все клавиши матрицы
  for (uint8_t col = 0; col < 5; col++) {
    for (uint8_t row = 0; row < 4; row++) {
      key_code_t key = g_keymap[col][row];
      if (key != KEY_NONE) {
        bool is_pressed = (key == matrix_key);
        process_key_fsm(key, is_pressed);
      }
    }
  }

  // Обработать PTT отдельно
  bool ptt_pressed = scan_ptt();
  process_key_fsm(KEY_PTT, ptt_pressed);
}

bool keyboard_is_pressed(key_code_t key) {
  if (key >= KEY_COUNT) {
    return false;
  }
  return g_keys[key].physical_state;
}
