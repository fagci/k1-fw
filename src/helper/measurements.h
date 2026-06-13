#ifndef MEASUREMENTS_H
#define MEASUREMENTS_H

#include "../misc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SQ_PRESETS_COUNT 11  // 0..10 levels
#define SQ_HYSTERESIS 4      // Отклонение close от open thresholds

typedef struct {
  uint16_t ro;  // RSSI open threshold
  uint8_t no;   // Noise open threshold
  uint8_t go;   // Glitch open threshold
  uint8_t rc;   // RSSI close threshold (ro - 4)
  uint8_t nc;   // Noise close threshold (no + 4)
  uint8_t gc;   // Glitch close threshold (go + 4)
} SquelchPreset;

static const uint16_t RSSI_MIN = 28;
static const uint16_t RSSI_MAX = 226;

typedef struct {
  uint16_t ro;
  uint16_t rc;
  uint8_t no;
  uint8_t nc;
  uint8_t go;
  uint8_t gc;
} SQL;

static const uint8_t rssi2s[2][15] = {
    // VHF
    {121, 115, 109, 103, 97, 91, 85, 79, 73, 63, 53, 43, 33, 23, 13},
    // UHF
    {141, 135, 129, 123, 117, 111, 105, 99, 93, 83, 73, 63, 53, 43, 33},
};

int32_t Clamp(int32_t v, int32_t min, int32_t max);
int ConvertDomain(int aValue, int aMin, int aMax, int bMin, int bMax);
uint8_t Rssi2PX(uint16_t rssi, uint8_t pxMin, uint8_t pxMax);
uint8_t DBm2S(int dbm, bool isUHF);
int16_t Rssi2DBm(uint16_t rssi);
uint16_t DBm2Rssi(int16_t dbm);
uint16_t Min(const uint16_t *array, size_t n);
uint16_t Max(const uint16_t *array, size_t n);
uint16_t Mean(const uint16_t *array, size_t n);
uint16_t Std(const uint16_t *data, size_t n);

uint32_t AdjustU(uint32_t val, uint32_t min, uint32_t max, int32_t inc);
uint32_t IncDecU(uint32_t val, uint32_t min, uint32_t max, bool inc);

bool IsReadable(const char *name);
SQL GetSql(uint8_t level);

// Squelch preset management (file-based, VHF/UHF separate)
void SQ_InitPresets(void);                          // Call at system startup
SquelchPreset GetSqlPreset(uint8_t level, uint32_t freq);  // Get preset for level+freq
bool SQ_SavePreset(const char *file, uint8_t level, SquelchPreset *preset);
bool SQ_LoadPreset(const char *file, uint8_t level, SquelchPreset *preset);

uint32_t DeltaF(uint32_t f1, uint32_t f2);
uint32_t RoundToStep(uint32_t f, uint32_t step);

#endif /* end of include guard: MEASUREMENTS_H */
