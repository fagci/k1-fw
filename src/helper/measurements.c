#include "measurements.h"
#include "../settings.h"
#include "../helper/storage.h"
#include <stdint.h>

#define SQ_HYSTERESIS 4  // Отклонение close от open thresholds

static const char *SQ_FILE_VHF = "/vhf.sq";
static const char *SQ_FILE_UHF = "/uhf.sq";

int32_t Clamp(int32_t v, int32_t min, int32_t max) {
  return v <= min ? min : (v >= max ? max : v);
}

int ConvertDomain(int aValue, int aMin, int aMax, int bMin, int bMax) {
  const int aRange = aMax - aMin;
  const int bRange = bMax - bMin;
  aValue = Clamp(aValue, aMin, aMax);
  return ((aValue - aMin) * bRange + aRange / 2) / aRange + bMin;
}

uint8_t DBm2S(int dbm, bool isUHF) {
  uint8_t i = 0;
  dbm *= -1;
  for (i = 0; i < 15; i++) {
    if (dbm >= rssi2s[isUHF][i]) {
      return i;
    }
  }
  return i;
}

int16_t Rssi2DBm(uint16_t rssi) { return (rssi >> 1) - 160; }
uint16_t DBm2Rssi(int16_t dbm) { return (dbm + 160) << 1; }

// applied x2 to prevent initial rounding
uint8_t Rssi2PX(uint16_t rssi, uint8_t pxMin, uint8_t pxMax) {
  return ConvertDomain(rssi - 320, -260, -120, pxMin, pxMax);
}

uint16_t Min(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint16_t min = array[0];
  for (size_t i = 1; i < n; ++i) {
    if (array[i] < min) {
      min = array[i];
    }
  }
  return min;
}

uint16_t Max(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint16_t max = array[0];
  for (size_t i = 1; i < n; ++i) {
    if (array[i] > max) {
      max = array[i];
    }
  }
  return max;
}

uint16_t Mean(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint32_t sum = array[0];
  for (size_t i = 1; i < n; ++i) {
    sum += array[i];
  }
  return sum / n;
}

uint32_t AdjustU(uint32_t val, uint32_t min, uint32_t max, int32_t inc) {
  if (inc > 0) {
    return val == max - inc ? min : val + inc;
  } else {
    return val > min ? val + inc : max + inc;
  }
}

uint32_t IncDecU(uint32_t val, uint32_t min, uint32_t max, bool inc) {
  return AdjustU(val, min, max, inc ? 1 : -1);
}

bool IsReadable(const char *name) { return name[0] >= 32 && name[0] < 127; }

SQL GetSql(uint8_t level) {
  SQL sq = {0, 0, 255, 255, 255, 255};
  if (level == 0) {
    return sq;
  }

  // sq.ro = ConvertDomain(level, 0, 10, 10, 180);
  sq.ro = ConvertDomain(level, 0, 10, 60, 180);
  sq.no = ConvertDomain(level, 0, 10, 64, 12);
  sq.go = ConvertDomain(level, 0, 10, 100, 0); // was 32, 6

  sq.rc = sq.ro - SQ_HYSTERESIS;
  sq.nc = sq.no + SQ_HYSTERESIS;
  sq.gc = sq.go + SQ_HYSTERESIS;
  return sq;
}

// ── Squelch preset file-based management ─────────────────────────────────────

static void SQ_InitDefaults(SquelchPreset *presets) {
  for (uint8_t i = 0; i < SQ_PRESETS_COUNT; i++) {
    SQL sq = GetSql(i);
    presets[i].ro = sq.ro;
    presets[i].no = sq.no;
    presets[i].go = sq.go;
    presets[i].rc = sq.rc;
    presets[i].nc = sq.nc;
    presets[i].gc = sq.gc;
  }
}

static void SQ_EnsureFileExists(const char *file) {
  SquelchPreset p;
  if (!SQ_LoadPreset(file, 0, &p)) {
    // File doesn't exist - create with defaults
    SquelchPreset presets[SQ_PRESETS_COUNT];
    SQ_InitDefaults(presets);
    for (uint8_t i = 0; i < SQ_PRESETS_COUNT; i++) {
      SQ_SavePreset(file, i, &presets[i]);
    }
  }
}

void SQ_InitPresets(void) {
  SQ_EnsureFileExists(SQ_FILE_VHF);
  SQ_EnsureFileExists(SQ_FILE_UHF);
}

SquelchPreset GetSqlPreset(uint8_t level, uint32_t freq) {
  if (level >= SQ_PRESETS_COUNT) level = SQ_PRESETS_COUNT - 1;
  
  bool isUHF = (freq >= SETTINGS_GetFilterBound());
  const char *file = isUHF ? SQ_FILE_UHF : SQ_FILE_VHF;
  
  SquelchPreset preset;
  if (SQ_LoadPreset(file, level, &preset)) {
    return preset;
  }
  
  // Fallback to calculated default
  SQL sq = GetSql(level);
  preset.ro = sq.ro;
  preset.no = sq.no;
  preset.go = sq.go;
  preset.rc = sq.rc;
  preset.nc = sq.nc;
  preset.gc = sq.gc;
  return preset;
}

bool SQ_SavePreset(const char *file, uint8_t level, SquelchPreset *preset) {
  if (level >= SQ_PRESETS_COUNT) return false;
  return Storage_Save(file, level, preset, sizeof(SquelchPreset));
}

bool SQ_LoadPreset(const char *file, uint8_t level, SquelchPreset *preset) {
  if (level >= SQ_PRESETS_COUNT) return false;
  return Storage_Load(file, level, preset, sizeof(SquelchPreset));
}

uint32_t DeltaF(uint32_t f1, uint32_t f2) {
  return f1 > f2 ? f1 - f2 : f2 - f1;
}

uint32_t RoundToStep(uint32_t f, uint32_t step) {
  uint32_t sd = f % step;
  if (sd > step / 2) {
    f += step - sd;
  } else {
    f -= sd;
  }
  return f;
}
