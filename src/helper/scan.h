#ifndef SCAN_H
#define SCAN_H

#include "../helper/measurements.h"
#include "../helper/scancommand.h"
#include "../inc/band.h"
#include <stdbool.h>
#include <stdint.h>

// Состояния машины состояний сканирования
typedef enum {
  SCAN_STATE_IDLE, // Ждём команды (только в командном режиме)
  SCAN_STATE_TUNING, // Переключаем частоту и ждём warmup
  SCAN_STATE_CHECKING, // Ждём подтверждения и проверяем squelch железом
  SCAN_STATE_LISTENING, // Squelch открыт, слушаем сигнал
} ScanState;

// Режимы работы сканера
typedef enum {
  SCAN_MODE_NONE,
  SCAN_MODE_SINGLE, // Мониторинг одной частоты (VFO)
  SCAN_MODE_FREQUENCY, // Частотное сканирование по диапазону
  SCAN_MODE_CHANNEL,    // Сканирование по каналам
  SCAN_MODE_ANALYSER,   // Анализатор спектра
  SCAN_MODE_MULTIWATCH, // Мультивотч нескольких VFO
} ScanMode;

#define NOISE_HISTORY_SIZE 32

typedef struct {
  uint8_t values[NOISE_HISTORY_SIZE];
  uint8_t idx;
  uint8_t count;
  uint16_t sum;
  uint32_t sum_sq; // сумма квадратов для stddev
  uint8_t mean;
  uint8_t stddev;
  uint8_t k; // коэффициент (2 по умолчанию)
} NoiseHistory;

// Контекст сканирования
typedef struct {
  // Машина состояний
  ScanState state;
  ScanMode mode;
  uint32_t stateEnteredAt; // Время входа в текущее состояние (из ChangeState)

  // Диапазон сканирования
  uint32_t currentF;
  uint32_t startF;
  uint32_t endF;
  uint16_t stepF;
  bool cmdRangeActive; // В командном режиме: выполняем range-команду

  // Squelch
  uint32_t squelchLevel; // Программный порог (авто-адаптивный)
  Measurement measurement;
  bool isOpen; // Текущее состояние squelch

  // Таймауты
  uint32_t warmupUs; // Задержка после переключения частоты (warmup)

  // Статистика
  uint32_t scanCycles;
  uint32_t currentCps;
  uint32_t lastCpsTime;
  uint32_t radioTimer;
  uint8_t idleCycles; // Циклы без открытого squelch (для авто-снижения порога)

  // Командный режим
  SCMD_Context *cmdCtx;

  bool precise;

  NoiseHistory noiseHist;
  uint8_t adaptiveThreshold; // вычисляемый порог

} ScanContext;
typedef enum {
  SCAN_ALGO_ADAPTIVE = 0,
  SCAN_ALGO_FULLRESET,
  SCAN_ALGO_STATISTICAL,
  SCAN_ALGO_CALIBRATED,
  SCAN_ALGO_COUNT
} ScanAlgo;

// ============================================================================
// API
// ============================================================================

void SCAN_Init(void);
void SCAN_SetMode(ScanMode mode);
ScanMode SCAN_GetMode(void);

void SCAN_Check(void); // Главный цикл обновления

// Командный режим
void SCAN_LoadCommandFile(const char *filename);
void SCAN_SetCommandMode(bool enabled);
bool SCAN_IsCommandMode(void);
void SCAN_CommandForceNext(void);

// Управление диапазоном
void SCAN_SetBand(Band b);
void SCAN_SetStartF(uint32_t f);
void SCAN_SetEndF(uint32_t f);
void SCAN_SetRange(uint32_t fs, uint32_t fe);

// Действия
void SCAN_Next(void);
void SCAN_NextBlacklist(void);
void SCAN_NextWhitelist(void);

// Параметры
void SCAN_SetDelay(uint32_t delay);
uint32_t SCAN_GetDelay(void);
uint32_t SCAN_GetCps(void);

// Инспекция командного режима
SCMD_Command *SCAN_GetCurrentCommand(void);
SCMD_Command *SCAN_GetNextCommand(void);
uint16_t SCAN_GetCommandIndex(void);
uint16_t SCAN_GetCommandCount(void);
uint16_t SCAN_GetSquelchLevel(void);

void SCAN_HandleInterrupt(uint16_t int_bits);
bool SCAN_IsSqOpen(void);
bool SCAN_IsSweeping(void);
const char *SCAN_GetStateName(void);

extern const char *SCAN_MODE_NAMES[];
extern const char *SCAN_STATE_NAMES[];
ScanState SCAN_GetState(void);

#endif
