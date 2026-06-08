#include "bk4829.h"

#include "../external/printf/printf.h"
#include "../helper/measurements.h"
#include "../settings.h"
#include "bk4819-regs.h"
#include "gpio.h"
#include "py32f071_ll_spi.h"
#include "systick.h"
#include <stdint.h>

// ============================================================================
// Tables
// ============================================================================

static const uint16_t MOD_TYPE_REG47_VALUES[] = {
    [MOD_FM] = BK4819_AF_FM,      [MOD_AM] = BK4819_AF_FM,
    [MOD_LSB] = BK4819_AF_USB,    [MOD_USB] = BK4819_AF_USB,
    [MOD_BYP] = BK4819_AF_BYPASS, [MOD_RAW] = BK4819_AF_RAW,
    [MOD_WFM] = BK4819_AF_FM,
};

static const uint8_t SQUELCH_TYPE_VALUES[4] = {0x88, 0xAA, 0xCC, 0xFF};

static const uint8_t DTMF_COEFFS[] = {
    111, 107, 103, 98, 80, 71, 58, 44, 65, 55, 37, 23, 228, 203, 181, 159,
};

const Gain GAIN_TABLE[32] = {
    {0x3ff, 0},  {0x3ff, 0},  {0x3f7, 3},  {0x3ef, 6},  {0x3e7, 8},
    {0x3e6, 11}, {0x3e5, 14}, {0x3e4, 17}, {0x3d3, 20}, {0x3b3, 22},
    {0x3c3, 25}, {0x3b2, 28}, {0x3c2, 31}, {0x3b1, 34}, {0x3f0, 36},
    {0x3e8, 39}, {0x390, 42}, {0x3a0, 45}, {0x368, 48}, {0x360, 50},
    {0x348, 53}, {0x2a0, 56}, {0x301, 59}, {0x20a, 62}, {0x248, 64},
    {0x10a, 67}, {0x201, 70}, {0x109, 73}, {0x200, 76}, {0x1, 78},
    {0x100, 81}, {0x0, 84},
};

typedef struct {
  uint8_t lo, low, high;
} AgcConfig;
static const AgcConfig AGC_DEFAULT = {0, 56, 84};
static const AgcConfig AGC_FAST = {0, 32, 50};

// ============================================================================
// State
// ============================================================================

static bool isInitialized = false;
static uint16_t reg30_cache = 0;
static bool reg30_cached = false;

static uint16_t gGpioOutState = 0x9000;
static uint8_t gSelectedFilter = 255;
static ModulationType gLastModulation = 255;
static uint16_t gFreqCacheLow = 0xFFFF;
static uint16_t gFreqCacheHigh = 0xFFFF;

// write-through cache for RMW-heavy registers
static uint16_t gRegCache_43 = 0xFFFF; // REG_43 filter BW
static uint16_t gRegCache_47 = 0xFFFF; // REG_47 AF mode
static uint16_t gRegCache_7E = 0xFFFF; // REG_7E AGC
static uint16_t gRegCache_73 = 0xFFFF; // REG_73 AFC

// ============================================================================
// SPI (bit-bang)
// ============================================================================

#define PIN_CSN GPIO_MAKE_PIN(GPIOF, LL_GPIO_PIN_9)
#define PIN_SCL GPIO_MAKE_PIN(GPIOB, LL_GPIO_PIN_8)
#define PIN_SDA GPIO_MAKE_PIN(GPIOB, LL_GPIO_PIN_9)

#define CS_PORT GPIO_PORT(PIN_CSN)
#define CS_MASK GPIO_PIN_MASK(PIN_CSN)
#define SCL_PORT GPIO_PORT(PIN_SCL)
#define SCL_MASK GPIO_PIN_MASK(PIN_SCL)
#define SDA_PORT GPIO_PORT(PIN_SDA)
#define SDA_MASK GPIO_PIN_MASK(PIN_SDA)

static inline void CS_Low(void) { CS_PORT->BSRR = (uint32_t)CS_MASK << 16; }
static inline void CS_High(void) { CS_PORT->BSRR = CS_MASK; }
static inline void SCL_Low(void) { SCL_PORT->BSRR = (uint32_t)SCL_MASK << 16; }
static inline void SCL_High(void) { SCL_PORT->BSRR = SCL_MASK; }
static inline void SDA_Low(void) { SDA_PORT->BSRR = (uint32_t)SDA_MASK << 16; }
static inline void SDA_High(void) { SDA_PORT->BSRR = SDA_MASK; }

static inline void SDA_AsOutput(void) {
  LL_GPIO_SetPinMode(SDA_PORT, SDA_MASK, LL_GPIO_MODE_OUTPUT);
}
static inline void SDA_AsInput(void) {
  LL_GPIO_SetPinMode(SDA_PORT, SDA_MASK, LL_GPIO_MODE_INPUT);
}
static inline uint32_t SDA_Read(void) {
  return (SDA_PORT->IDR & SDA_MASK) ? 1u : 0u;
}
static inline void SDA_WriteBit(uint32_t bit) {
  SDA_PORT->BSRR = bit ? SDA_MASK : ((uint32_t)SDA_MASK << 16);
}

static inline uint16_t scale_frequency(uint16_t freq) {
  return (((uint32_t)freq * 1353245u) + (1u << 16)) >> 17;
}

static inline void BK4819_WriteU8(uint8_t data) {
  for (unsigned i = 0; i < 8; ++i) {
    SCL_Low();
    SDA_WriteBit(data & 0x80u);
    SCL_High();
    data <<= 1;
  }
}

static inline void BK4819_WriteU16(uint16_t data) {
  for (unsigned i = 0; i < 16; ++i) {
    SCL_Low();
    SDA_WriteBit(data & 0x8000u);
    SCL_High();
    data <<= 1;
  }
}

static uint16_t BK4819_ReadU16(void) {
  uint16_t value = 0;
  SDA_AsInput();
  SCL_Low();
  __asm volatile("nop\nnop\nnop\nnop\nnop\n"); // ~1us
  for (int i = 0; i < 16; i++) {
    SCL_High();
    __asm volatile("nop");
    value = (value << 1) | SDA_Read();
    SCL_Low();
    __asm volatile("nop");
  }
  SDA_High();
  SDA_AsOutput();
  return value;
}

// ============================================================================
// Register Cache
// ============================================================================

static inline void _UpdateRegCache(BK4819_REGISTER_t reg, uint16_t data) {
  switch (reg) {
  case BK4819_REG_43:
    gRegCache_43 = data;
    break;
  case BK4819_REG_47:
    gRegCache_47 = data;
    break;
  case BK4819_REG_7E:
    gRegCache_7E = data;
    break;
  case 0x73:
    gRegCache_73 = data;
    break;
  default:
    break;
  }
}

static inline uint16_t _ReadRegCached(BK4819_REGISTER_t reg) {
  switch (reg) {
  case BK4819_REG_43:
    if (gRegCache_43 != 0xFFFF)
      return gRegCache_43;
    break;
  case BK4819_REG_47:
    if (gRegCache_47 != 0xFFFF)
      return gRegCache_47;
    break;
  case BK4819_REG_7E:
    if (gRegCache_7E != 0xFFFF)
      return gRegCache_7E;
    break;
  case 0x73:
    if (gRegCache_73 != 0xFFFF)
      return gRegCache_73;
    break;
  default:
    break;
  }
  return BK4819_ReadRegister(reg);
}

// ============================================================================
// Register Access
// ============================================================================

uint16_t BK4819_ReadRegister(BK4819_REGISTER_t reg) {
  if (reg == BK4819_REG_30 && reg30_cached)
    return reg30_cache;

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  CS_High();
  __asm volatile("");
  CS_Low();
  BK4819_WriteU8(reg | 0x80);
  uint16_t value = BK4819_ReadU16();
  CS_High();
  SCL_High();
  SDA_High();

  __set_PRIMASK(primask);
  return value;
}

void BK4819_WriteRegister(BK4819_REGISTER_t reg, uint16_t data) {
  // Фильтруем дублирующие записи для кэшируемых регистров
  switch (reg) {
  case BK4819_REG_30:
    if (reg30_cached && reg30_cache == data)
      return;
    reg30_cache = data;
    reg30_cached = true;
    break;
  case BK4819_REG_43:
    if (gRegCache_43 != 0xFFFF && gRegCache_43 == data)
      return;
    gRegCache_43 = data;
    break;
  case BK4819_REG_47:
    if (gRegCache_47 != 0xFFFF && gRegCache_47 == data)
      return;
    gRegCache_47 = data;
    break;
  case BK4819_REG_7E:
    if (gRegCache_7E != 0xFFFF && gRegCache_7E == data)
      return;
    gRegCache_7E = data;
    break;
  case 0x73:
    if (gRegCache_73 != 0xFFFF && gRegCache_73 == data)
      return;
    gRegCache_73 = data;
    break;
  default:
    break;
  }

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  CS_High();
  __asm volatile("");
  CS_Low();
  BK4819_WriteU8(reg);
  BK4819_WriteU16(data);
  CS_High();
  SCL_High();
  SDA_High();

  __set_PRIMASK(primask);
}

uint16_t BK4819_GetRegValue(RegisterSpec spec) {
  return (_ReadRegCached(spec.num) >> spec.offset) & spec.mask;
}

void BK4819_SetRegValue(RegisterSpec spec, uint16_t value) {
  uint16_t reg = _ReadRegCached(spec.num);
  reg &= ~(spec.mask << spec.offset);
  BK4819_WriteRegister(spec.num, reg | (value << spec.offset));
}

// ============================================================================
// Initialization
// ============================================================================

#define XTAL26M 0
#define XTAL13M 1
#define XTAL19M2 2
#define XTAL12M8 3
#define XTAL25M6 4
#define XTAL38M4 5

void RF_SetXtal(uint8_t mode) {
  const uint16_t dev = gSettings.deviation * 10;
#define R40 0x3000
  switch (mode) {
  case XTAL26M:
    BK4819_WriteRegister(0x40, R40 | dev);
    break;
  case XTAL13M:
    BK4819_WriteRegister(0x40, R40 | dev);
    BK4819_WriteRegister(0x41, 0x81C1);
    BK4819_WriteRegister(0x3B, 0xAC40);
    BK4819_WriteRegister(0x3C, 0x2708);
    BK4819_WriteRegister(0x1D, 0x3555); // BPF
    break;
  case XTAL19M2:
    BK4819_WriteRegister(0x40, R40 | dev);
    BK4819_WriteRegister(0x41, 0x81C2);
    BK4819_WriteRegister(0x3B, 0x9800);
    BK4819_WriteRegister(0x3C, 0x3A48);
    BK4819_WriteRegister(0x1D, 0x2E39); // BPF
    break;
  case XTAL12M8:
    BK4819_WriteRegister(0x40, R40 | dev);
    BK4819_WriteRegister(0x41, 0x81C1);
    BK4819_WriteRegister(0x3B, 0x1000);
    BK4819_WriteRegister(0x3C, 0x2708);
    BK4819_WriteRegister(0x1D, 0x3555); // BPF
    break;
  case XTAL25M6:
    BK4819_WriteRegister(0x3B, 0x2000);
    BK4819_WriteRegister(0x3C, 0x4E88);
    break;
  case XTAL38M4:
    BK4819_WriteRegister(0x40, R40 | dev);
    BK4819_WriteRegister(0x41, 0x81C5);
    BK4819_WriteRegister(0x3B, 0x3000);
    BK4819_WriteRegister(0x3C, 0x75C8);
    BK4819_WriteRegister(0x1D, 0x261C); // BPF
    break;
  }
#undef R40
}

void BK4819_Init(void) {
  if (isInitialized)
    return;

  gSelectedFilter = 255;
  gLastModulation = 255;
  gFreqCacheLow = 0xFFFF;
  gFreqCacheHigh = 0xFFFF;
  gRegCache_43 = 0xFFFF;
  gRegCache_47 = 0xFFFF;
  gRegCache_7E = 0xFFFF;
  gRegCache_73 = 0xFFFF;
  reg30_cached = false;

  CS_High();
  SCL_High();
  SDA_High();

  // снижаем slew rate — уменьшает гармоники bit-bang SPI в RF диапазоне
  LL_GPIO_SetPinSpeed(SCL_PORT, SCL_MASK, LL_GPIO_SPEED_FREQ_MEDIUM);
  LL_GPIO_SetPinSpeed(SDA_PORT, SDA_MASK, LL_GPIO_SPEED_FREQ_LOW);
  LL_GPIO_SetPinSpeed(CS_PORT, CS_MASK, LL_GPIO_SPEED_FREQ_LOW);

  BK4819_WriteRegister(BK4819_REG_00, 0x8000); // reset
  BK4819_WriteRegister(BK4819_REG_00, 0x0000);

  BK4819_WriteRegister(BK4819_REG_37, 0x9D1F);
  BK4819_WriteRegister(BK4819_REG_36, 0x0022); // PA bias off

  BK4819_WriteRegister(BK4819_REG_10, 0x0318);
  BK4819_WriteRegister(BK4819_REG_11, 0x033A);
  BK4819_WriteRegister(BK4819_REG_12, 0x03DB);
  BK4819_WriteRegister(BK4819_REG_7B, 0x73DC);

  BK4819_WriteRegister(BK4819_REG_48,
                       (11u << 12) |   // unknown
                           (0 << 10) | // AF Rx Gain-1: 0dB
                           (58 << 4) | // AF Rx Gain-2
                           (8 << 0));  // AF DAC Gain

  RF_SetXtal(XTAL26M);

  for (unsigned i = 0; i < ARRAY_SIZE(DTMF_COEFFS); i++)
    BK4819_WriteRegister(BK4819_REG_09, (i << 12) | DTMF_COEFFS[i]);

  BK4819_WriteRegister(0x1C, 0x07C0);
  BK4819_WriteRegister(0x1D, 0xE555);
  BK4819_WriteRegister(0x1E, 0x4C58);
  BK4819_WriteRegister(0x1F, 0xC65A);

  BK4819_WriteRegister(BK4819_REG_3E, 0x94C6);

  BK4819_WriteRegister(0x73, 0x4691); // AFC off
  BK4819_WriteRegister(0x77, 0x88EF);

  BK4819_WriteRegister(BK4819_REG_7D, 0xE920); // mic sens
  BK4819_WriteRegister(BK4819_REG_19, 0x104E); // MIC AGC on
  BK4819_WriteRegister(BK4819_REG_28, 0x0B40); // RX noise gate
  BK4819_WriteRegister(BK4819_REG_29, 0xAA00); // TX noise gate

  BK4819_WriteRegister(0x2A, 0x6600); // audio gain1 tc
  BK4819_WriteRegister(0x2C, 0x1822); // audio emph tc, tx gain
  BK4819_WriteRegister(0x2F, 0x9890); // audio tx limit, emph rx gain
  BK4819_WriteRegister(0x53, 0x2028); // audio alc tc

  BK4819_WriteRegister(BK4819_REG_7E, 0x303E);
  BK4819_WriteRegister(BK4819_REG_46, 0x600A);
  BK4819_WriteRegister(0x4A, 0x5430);

  gGpioOutState = 0x9000;
  BK4819_WriteRegister(BK4819_REG_33, gGpioOutState);
  BK4819_WriteRegister(BK4819_REG_3F, 0);

  BK4819_SetupPowerAmplifier(0, 0);
  BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);

  BK4819_WriteRegister(BK4819_REG_43, 0x3028);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 1);

  isInitialized = true;
}

// ============================================================================
// Utility
// ============================================================================

void BK4819_Idle(void) { BK4819_WriteRegister(BK4819_REG_30, 0x0000); }

void BK4819_Sleep(void) {
  BK4819_Idle();
  BK4819_WriteRegister(BK4819_REG_37, 0x1D00);
}

void BK4819_SetToneFrequency(uint16_t freq) {
  BK4819_WriteRegister(BK4819_REG_71, scale_frequency(freq));
}
void BK4819_SetTone2Frequency(uint16_t freq) {
  BK4819_WriteRegister(BK4819_REG_72, scale_frequency(freq));
}

void BK4819_EnterTxMute(void) { BK4819_WriteRegister(BK4819_REG_50, 0xBB18); }
void BK4819_ExitTxMute(void) { BK4819_WriteRegister(BK4819_REG_50, 0x3B18); }
void BK4819_ExitSubAu(void) { BK4819_WriteRegister(BK4819_REG_51, 0x0000); }
void BK4819_DisableDTMF(void) { BK4819_WriteRegister(BK4819_REG_24, 0); }

bool BK4819_IsSquelchOpen(void) {
  return (BK4819_ReadRegister(BK4819_REG_0C) >> 1) & 1;
}

void BK4819_MuteMic(void) {
  BK4819_WriteRegister(BK4819_REG_30,
                       BK4819_ReadRegister(BK4819_REG_30) & ~(1u << 2));
}

// ============================================================================
// GPIO / Filter
// ============================================================================

void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t pin, bool enable) {
  const uint16_t bit = 0x40U >> pin;
  if (enable) {
    gGpioOutState |= bit;
  } else {
    gGpioOutState &= ~bit;
  }
  BK4819_WriteRegister(BK4819_REG_33, gGpioOutState);
}

void BK4819_SelectFilterEx(Filter filter) {
  if (gSelectedFilter == filter)
    return;
  gSelectedFilter = filter;

  const uint16_t BIT_VHF = 0x40U >> BK4819_GPIO4_PIN32_VHF_LNA;
  const uint16_t BIT_UHF = 0x40U >> BK4819_GPIO3_PIN31_UHF_LNA;

  if (filter == FILTER_VHF) {
    gGpioOutState |= BIT_VHF;
  } else {
    gGpioOutState &= ~BIT_VHF;
  }
  if (filter == FILTER_UHF) {
    gGpioOutState |= BIT_UHF;
  } else {
    gGpioOutState &= ~BIT_UHF;
  }

  BK4819_WriteRegister(BK4819_REG_33, gGpioOutState);
}

void BK4819_SelectFilter(uint32_t frequency) {
  BK4819_SelectFilterEx((frequency < SETTINGS_GetFilterBound()) ? FILTER_VHF
                                                                : FILTER_UHF);
}

// ============================================================================
// AGC
// ============================================================================

int8_t BK4819_GetAgcIndex(void) {
  int8_t idx = (BK4819_ReadRegister(BK4819_REG_7E) >> 12) & 7;
  return (idx > 3) ? idx - 8 : idx;
}

uint8_t BK4819_GetAttenuation(void) {
  static const BK4819_REGISTER_t idx_to_reg[] = {
      BK4819_REG_10,
      BK4819_REG_11,
      BK4819_REG_12,
      BK4819_REG_13,
  };
  static const uint8_t lna_peak[4] = {19, 16, 11, 0};
  static const uint8_t lna_gain[8] = {24, 19, 14, 9, 6, 4, 2, 0};
  static const uint8_t mixer_gain[4] = {8, 6, 3, 0};
  static const uint8_t pga_gain[8] = {33, 27, 21, 15, 9, 6, 3, 0};

  int8_t idx = BK4819_GetAgcIndex();
  BK4819_REGISTER_t reg = (idx == -1) ? BK4819_REG_14 : idx_to_reg[idx];

  uint16_t v = BK4819_ReadRegister(reg);
  return lna_peak[(v >> 8) & 3] + lna_gain[(v >> 5) & 7] +
         mixer_gain[(v >> 3) & 3] + pga_gain[v & 7];
}

void BK4819_SetAGC(bool fm, uint8_t gainIndex) {
  const bool enableAgc = (gainIndex == AUTO_GAIN_INDEX);
  const AgcConfig *cfg = fm ? &AGC_DEFAULT : &AGC_FAST;

  uint16_t reg7E = _ReadRegCached(BK4819_REG_7E);
  reg7E &= ~((1 << 15) | (0b111 << 12));
  reg7E |= (!enableAgc << 15) | (3u << 12); // | (5u << 3); // | (6u << 0);

  uint16_t reg49 =
      fm ? 0x2AB2 : (uint16_t)((cfg->lo << 14) | (cfg->high << 7) | cfg->low);

  BK4819_WriteRegister(BK4819_REG_13,
                       enableAgc ? 0x03DF : GAIN_TABLE[gainIndex].regValue);
  BK4819_WriteRegister(BK4819_REG_14, fm ? 0x0210 : 0x0000);
  BK4819_WriteRegister(BK4819_REG_49, reg49);
  BK4819_WriteRegister(BK4819_REG_7E, reg7E);
}

// ============================================================================
// Power Amplifier / Frequency
// ============================================================================

void BK4819_SetupPowerAmplifier(uint8_t bias, uint32_t frequency) {
  uint8_t gain = (frequency < VHF_UHF_BOUND2) ? 0x08 : 0x22;
  BK4819_WriteRegister(BK4819_REG_36, (bias << 8) | 0x80U | gain);
}

void BK4819_SetFrequency(uint32_t freq) {
  freq += gSettings.freqCorrection;
  uint16_t low = freq & 0xFFFF;
  uint16_t high = (freq >> 16) & 0xFFFF;
  if (low != gFreqCacheLow) {
    BK4819_WriteRegister(BK4819_REG_38, low);
    gFreqCacheLow = low;
  }
  if (high != gFreqCacheHigh) {
    BK4819_WriteRegister(BK4819_REG_39, high);
    gFreqCacheHigh = high;
  }
}

uint32_t BK4819_GetFrequency(void) {
  return ((uint32_t)BK4819_ReadRegister(BK4819_REG_39) << 16) |
         BK4819_ReadRegister(BK4819_REG_38);
}

void BK4819_TuneTo(uint32_t freq, bool precise) {
  BK4819_SetFrequency(freq);
  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
  BK4819_WriteRegister(BK4819_REG_30,
                       precise ? 0 : (reg & ~BK4819_REG_30_ENABLE_VCO_CALIB));
  BK4819_WriteRegister(BK4819_REG_30, reg);
}

// ============================================================================
// Xtal / IF mode
// ============================================================================

XtalMode BK4819_XtalGet(void) {
  return (XtalMode)((BK4819_ReadRegister(0x3C) >> 6) & 0b11);
}

void BK4819_XtalSet(XtalMode mode) {
  static const struct {
    uint16_t xtal, ifset;
  } cfg[] = {
      [XTAL_0_13M] = {20232, 0x3555},
      [XTAL_1_19_2M] = {20296, 0x2E39},
      [XTAL_2_26M] = {20360, 0x2AAB},
      [XTAL_3_38_4M] = {20424, 0x271C},
  };
  BK4819_WriteRegister(0x3C, cfg[mode].xtal);
  BK4819_WriteRegister(0x3D, cfg[mode].ifset);
}

void BK4819_SetIfMode(uint8_t mode) {
  static const struct {
    uint16_t r1c, r1d;
  } cfg[] = {
      [0] = {0x01C0, 0x0000}, // Zero IF
      [1] = {0x01C0, 0xE555}, // LPF
      [2] = {0x0122, 0x2AAB}, // BPF
  };
  if (mode > 2)
    return;
  BK4819_WriteRegister(0x1C, cfg[mode].r1c);
  BK4819_WriteRegister(0x1D, cfg[mode].r1d);
}

// ============================================================================
// Modulation / Filter BW
// ============================================================================

ModulationType BK4819_GetModulation(void) {
  uint16_t value = (BK4819_ReadRegister(BK4819_REG_47) >> 8) & 0xF;
  for (uint8_t i = 0; i < ARRAY_SIZE(MOD_TYPE_REG47_VALUES); ++i) {
    if (MOD_TYPE_REG47_VALUES[i] == value)
      return i;
  }
  return MOD_FM;
}

void BK4819_SetAF(BK4819_AF_Type_t af) {
  BK4819_WriteRegister(BK4819_REG_47, 0x6042 | (af << 8));
}

void BK4819_SetModulation(ModulationType type) {
  if (type == MOD_BYP) {
    BK4819_EnterBypass();
  } else if (gLastModulation == MOD_BYP) {
    BK4819_ExitBypass();
  }

  const bool isSsb = (type == MOD_LSB || type == MOD_USB);

  BK4819_SetAF(MOD_TYPE_REG47_VALUES[type]);
  BK4819_SetRegValue(RS_AFC_DIS, isSsb);

  if (type == MOD_WFM) {
    BK4819_WriteRegister(BK4819_REG_43,
                         (7u << 12) | (7u << 9) | (3u << 4) | (1u << 3));
    BK4819_XtalSet(XTAL_0_13M);
  } else {
    BK4819_XtalSet(XTAL_2_26M);
  }

  BK4819_WriteRegister(0x75, isSsb ? 0xFC13 : 0xF50B);

  if (isSsb) {
    BK4819_SetRegValue(RS_IF_F, 0);
  } else if (type == MOD_WFM) {
    BK4819_SetRegValue(RS_IF_F, 14223);
  } else {
    BK4819_SetRegValue(RS_IF_F, 10923);
  }

  uint16_t reg4A = (isSsb || type == MOD_AM) ? (0x5430 | 0x7F) : (0x5430 | 46);
  BK4819_WriteRegister(0x4A, reg4A);

  uint16_t r31 = BK4819_ReadRegister(0x31);
  if (type == MOD_AM) {
    BK4819_WriteRegister(0x31, r31 | 1);
    BK4819_WriteRegister(0x42, 0x6F5C);
    BK4819_WriteRegister(0x2A, 0x7434); // noise gate time constants
    BK4819_WriteRegister(0x2B, 0x0400);
    BK4819_WriteRegister(0x2F, 0x9990);
  } else {
    BK4819_WriteRegister(0x31, r31 & 0xFFFE);
    BK4819_WriteRegister(0x42, 0x6B5A);
    BK4819_WriteRegister(0x2A, 0x7400);
    BK4819_WriteRegister(0x2B, 0x0000);
    BK4819_WriteRegister(0x2F, 0x9890);
  }

  if (type == MOD_FM) {
    BK4819_WriteRegister(0x28, 0x0600); // noise gate FM
    BK4819_WriteRegister(0x2C, 0x6662); // emph/tx gain FM
  } else {
    BK4819_WriteRegister(0x28, 0x0B40);
    BK4819_WriteRegister(0x2C, 0x1822);
  }

  gLastModulation = type;
}

void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t bw) {
  if (bw > 9)
    return;

  // RF filter bandwidth (Apass=0.1dB)
  // 0=2k 1=2.5k 2=3k 3=3.5k 4=4kHz 5=4.5k 6=5k 7=5.5k
  // if REG_43 < 5 >= 1, RF filter bandwidth *= 2;
  //                           6  7  9 10 12 14 17 20 23 26
  static const uint8_t rf[] = {0, 1, 1, 3, 1, 2, 3, 4, 5, 7}; // Norm
  static const uint8_t wb[] = {0, 0, 1, 2, 1, 2, 2, 3, 4, 6}; // Weak

  // AF Tx LPF2 filter Band Width (Apass=1dB) Selection.
  // 1=2.5k 2=2.75k 0=3k 3=3.5k 7=4k 6=4.5k 5=5.0k 4=5.5k
  //                           6  7  9 10 12 14 17 20 23 26
  static const uint8_t af[] = {1, 2, 0, 3, 0, 0, 7, 6, 5, 4};

  // BW Mode Selection.
  // 0=12.5k 1=6.25k 2=25k/20k
  //                           6  7  9 10 12 14 17 20 23 26
  static const uint8_t bs[] = {1, 1, 0, 0, 2, 2, 2, 2, 2, 2};

  bool boost = bw < BK4819_FILTER_BW_12k;

  BK4819_WriteRegister(BK4819_REG_43, (rf[bw] << 12) | (wb[bw] << 9) |
                                          (af[bw] << 6) | (bs[bw] << 4) |
                                          (1u << 3) | ((boost ? 1 : 0) << 2));
}

// ============================================================================
// Squelch
// ============================================================================

void BK4819_SetupSquelch(SQL sq, uint8_t delayOpen, uint8_t delayClose) {
  sq.no = Clamp(sq.no, 0, 127);
  sq.nc = Clamp(sq.nc, 0, 127);
  BK4819_WriteRegister(BK4819_REG_4D, 0xA000 | sq.gc);
  BK4819_WriteRegister(BK4819_REG_4E, (1u << 14) | (delayOpen << 11) |
                                          (delayClose << 9) | (1 << 8) | sq.go);
  BK4819_WriteRegister(BK4819_REG_4F, (sq.nc << 8) | sq.no);
  BK4819_WriteRegister(BK4819_REG_78, (sq.ro << 8) | sq.rc);
}

void BK4819_Squelch(uint8_t sql, uint32_t freq, uint8_t openDelay,
                    uint8_t closeDelay) {
  SquelchPreset p = GetSqlPreset(sql, freq);
  SQL sq = {
      .ro = p.ro, .rc = p.rc, .no = p.no, .nc = p.nc, .go = p.go, .gc = p.gc};
  BK4819_SetupSquelch(sq, openDelay, closeDelay);
}

void BK4819_SquelchType(SquelchType type) {
  BK4819_SetRegValue(RS_SQ_TYPE, SQUELCH_TYPE_VALUES[type]);
}

// ============================================================================
// CTCSS / CDCSS
// ============================================================================

void BK4819_SetCDCSSCodeWord(uint32_t codeWord) {
  BK4819_WriteRegister(
      BK4819_REG_51,
      BK4819_REG_51_ENABLE_CxCSS | BK4819_REG_51_GPIO6_PIN2_NORMAL |
          BK4819_REG_51_TX_CDCSS_POSITIVE | BK4819_REG_51_MODE_CDCSS |
          BK4819_REG_51_CDCSS_23_BIT | BK4819_REG_51_1050HZ_NO_DETECTION |
          BK4819_REG_51_AUTO_CDCSS_BW_ENABLE |
          BK4819_REG_51_AUTO_CTCSS_BW_ENABLE |
          (51U << BK4819_REG_51_SHIFT_CxCSS_TX_GAIN1));
  BK4819_WriteRegister(BK4819_REG_07,
                       BK4819_REG_07_MODE_CTC1 |
                           (2775U << BK4819_REG_07_SHIFT_FREQUENCY));
  BK4819_WriteRegister(BK4819_REG_08, (codeWord >> 0) & 0xFFF);
  BK4819_WriteRegister(BK4819_REG_08, ((codeWord >> 12) & 0xFFF) | 0x8000);
}

void BK4819_SetCTCSSFrequency(uint32_t freqControlWord) {
  BK4819_WriteRegister(BK4819_REG_51,
                       (freqControlWord == 2625) ? 0x944A : 0x904A);
  BK4819_WriteRegister(BK4819_REG_07, BK4819_REG_07_MODE_CTC1 |
                                          (((freqControlWord * 2065) / 1000)
                                           << BK4819_REG_07_SHIFT_FREQUENCY));
}

void BK4819_SetTailDetection(uint32_t freq_10Hz) {
  BK4819_WriteRegister(BK4819_REG_07,
                       BK4819_REG_07_MODE_CTC2 |
                           ((253910 + (freq_10Hz / 2)) / freq_10Hz));
}

void BK4819_GenTail(uint8_t tail) {
  switch (tail) {
  case 0:
    BK4819_WriteRegister(BK4819_REG_52, 0x828F);
    break; // 134.4Hz
  case 1:
    BK4819_WriteRegister(BK4819_REG_52, 0xA28F);
    break; // 120°
  case 2:
    BK4819_WriteRegister(BK4819_REG_52, 0xC28F);
    break; // 180°
  case 3:
    BK4819_WriteRegister(BK4819_REG_52, 0xE28F);
    break; // 240°
  case 4:
    BK4819_WriteRegister(BK4819_REG_07, 0x046F);
    break; // 55Hz
  }
}

void BK4819_EnableCDCSS(void) {
  BK4819_GenTail(0);
  BK4819_WriteRegister(BK4819_REG_51, 0x804A);
}
void BK4819_EnableCTCSS(void) {
  BK4819_GenTail(4);
  BK4819_WriteRegister(BK4819_REG_51, 0x904A);
}

BK4819_CssScanResult_t BK4819_GetCxCSSScanResult(uint32_t *pCdcssFreq,
                                                 uint16_t *pCtcssFreq) {
  uint16_t high = BK4819_ReadRegister(BK4819_REG_69);
  if ((high & 0x8000) == 0) {
    uint16_t low = BK4819_ReadRegister(BK4819_REG_6A);
    *pCdcssFreq = ((high & 0xFFF) << 12) | (low & 0xFFF);
    return BK4819_CSS_RESULT_CDCSS;
  }
  uint16_t low = BK4819_ReadRegister(BK4819_REG_68);
  if ((low & 0x8000) == 0) {
    *pCtcssFreq = ((low & 0x1FFF) * 4843) / 10000;
    return BK4819_CSS_RESULT_CTCSS;
  }
  return BK4819_CSS_RESULT_NOT_FOUND;
}

uint8_t BK4819_GetCDCSSCodeType(void) {
  return (BK4819_ReadRegister(BK4819_REG_0C) >> 14) & 3;
}
uint8_t BK4819_GetCTCType(void) {
  return (BK4819_ReadRegister(BK4819_REG_0C) >> 10) & 3;
}

// ============================================================================
// DTMF
// ============================================================================

void BK4819_EnableDTMF(void) {
  BK4819_WriteRegister(BK4819_REG_21, 0x06D8);
  BK4819_WriteRegister(BK4819_REG_24,
                       (1U << BK4819_REG_24_SHIFT_UNKNOWN_15) |
                           (24 << BK4819_REG_24_SHIFT_THRESHOLD) |
                           (1U << BK4819_REG_24_SHIFT_UNKNOWN_6) |
                           BK4819_REG_24_ENABLE | BK4819_REG_24_SELECT_DTMF |
                           (14U << BK4819_REG_24_SHIFT_MAX_SYMBOLS));
}

void BK4819_PlayDTMF(char code) {
  static const struct {
    char c;
    uint16_t t1, t2;
  } map[] = {
      {'0', 0x25F3, 0x35E1}, {'1', 0x1C1C, 0x30C2}, {'2', 0x1C1C, 0x35E1},
      {'3', 0x1C1C, 0x3B91}, {'4', 0x1F0E, 0x30C2}, {'5', 0x1F0E, 0x35E1},
      {'6', 0x1F0E, 0x3B91}, {'7', 0x225C, 0x30C2}, {'8', 0x225C, 0x35E1},
      {'9', 0x225C, 0x3B91}, {'A', 0x1C1C, 0x41DC}, {'B', 0x1F0E, 0x41DC},
      {'C', 0x225C, 0x41DC}, {'D', 0x25F3, 0x41DC}, {'*', 0x25F3, 0x30C2},
      {'#', 0x25F3, 0x3B91},
  };
  for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
    if (map[i].c == code) {
      BK4819_WriteRegister(BK4819_REG_71, map[i].t1);
      BK4819_WriteRegister(BK4819_REG_72, map[i].t2);
      return;
    }
  }
}

void BK4819_PlayDTMFString(const char *string, bool delayFirst,
                           uint16_t firstPersist, uint16_t hashPersist,
                           uint16_t codePersist, uint16_t codeInterval) {
  for (uint8_t i = 0; string[i]; i++) {
    BK4819_PlayDTMF(string[i]);
    BK4819_ExitTxMute();
    uint16_t delay = (delayFirst && i == 0)                   ? firstPersist
                     : (string[i] == '*' || string[i] == '#') ? hashPersist
                                                              : codePersist;
    SYSTICK_DelayMs(delay);
    BK4819_EnterTxMute();
    SYSTICK_DelayMs(codeInterval);
  }
}

void BK4819_EnterDTMF_TX(bool localLoopback) {
  BK4819_EnableDTMF();
  BK4819_EnterTxMute();
  BK4819_SetAF(localLoopback ? BK4819_AF_BEEP : BK4819_AF_MUTE);
  BK4819_WriteRegister(BK4819_REG_70,
                       BK4819_REG_70_MASK_ENABLE_TONE1 |
                           (83 << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN) |
                           BK4819_REG_70_MASK_ENABLE_TONE2 |
                           (83 << BK4819_REG_70_SHIFT_TONE2_TUNING_GAIN));
  BK4819_EnableTXLink();
}

void BK4819_ExitDTMF_TX(bool keep) {
  BK4819_EnterTxMute();
  BK4819_SetAF(BK4819_AF_MUTE);
  BK4819_WriteRegister(BK4819_REG_70, 0x0000);
  BK4819_DisableDTMF();
  BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
  if (!keep)
    BK4819_ExitTxMute();
}

void BK4819_PlayDTMFEx(bool localLoopback, char code) {
  BK4819_EnableDTMF();
  BK4819_EnterTxMute();
  BK4819_SetAF(localLoopback ? BK4819_AF_BEEP : BK4819_AF_MUTE);
  BK4819_WriteRegister(BK4819_REG_70, 0xD3D3);
  BK4819_EnableTXLink();
  SYSTICK_DelayMs(50);
  BK4819_PlayDTMF(code);
  BK4819_ExitTxMute();
}

uint8_t BK4819_GetDTMF_5TONE_Code(void) {
  return (BK4819_ReadRegister(BK4819_REG_0B) >> 8) & 0x0F;
}

// ============================================================================
// Tone / Audio
// ============================================================================

void BK4819_PlayTone(uint16_t frequency, bool tuningGainSwitch) {
  BK4819_EnterTxMute();
  BK4819_SetAF(BK4819_AF_BEEP);
  BK4819_WriteRegister(BK4819_REG_70,
                       BK4819_REG_70_ENABLE_TONE1 |
                           ((tuningGainSwitch ? 28 : 96)
                            << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
  BK4819_Idle();
  BK4819_WriteRegister(BK4819_REG_30, BK4819_REG_30_ENABLE_AF_DAC |
                                          BK4819_REG_30_ENABLE_DISC_MODE |
                                          BK4819_REG_30_ENABLE_TX_DSP);
  BK4819_SetToneFrequency(frequency);
}

void BK4819_TransmitTone(uint32_t frequency) {
  BK4819_EnterTxMute();
  BK4819_WriteRegister(BK4819_REG_70,
                       BK4819_REG_70_MASK_ENABLE_TONE1 |
                           (56 << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
  BK4819_SetToneFrequency(frequency);
  BK4819_SetAF(BK4819_AF_MUTE);
  BK4819_EnableTXLink();
  BK4819_ExitTxMute();
}

void BK4819_PlayRogerTiny(void) {
  const uint16_t seq[] = {1250, 20, 0, 10, 1500, 20, 0, 0};
  BK4819_PlaySequence(seq);
}

void BK4819_PlaySequence(const uint16_t *sequence) {
  bool first = true;
  for (uint8_t i = 0; i < 255; i += 2) {
    uint16_t note = sequence[i];
    uint16_t duration = sequence[i + 1];
    if (!note && !duration)
      break;
    if (first) {
      first = false;
      BK4819_TransmitTone(note);
    } else {
      BK4819_SetToneFrequency(note);
      BK4819_ExitTxMute();
    }
    if (note && !duration)
      return;
    SYSTICK_DelayMs(duration);
  }
  BK4819_EnterTxMute();
}

void BK4819_ToggleAFBit(bool enable) {
  uint16_t reg = BK4819_ReadRegister(BK4819_REG_47);
  reg = enable ? (reg | (1 << 8)) : (reg & ~(1 << 8));
  BK4819_WriteRegister(BK4819_REG_47, reg);
}

void BK4819_ToggleAFDAC(bool enable) {
  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
  reg = enable ? (reg | BK4819_REG_30_ENABLE_AF_DAC)
               : (reg & ~BK4819_REG_30_ENABLE_AF_DAC);
  BK4819_WriteRegister(BK4819_REG_30, reg);
}

void BK4819_Enable_AfDac_DiscMode_TxDsp(void) {
  BK4819_Idle();
  BK4819_WriteRegister(BK4819_REG_30, 0x0302);
}

// ============================================================================
// TX / RX
// ============================================================================

void BK4819_RX_TurnOn(void) {
  BK4819_WriteRegister(BK4819_REG_37, 0x1D00 | 0x801F | (1 << 9));
  BK4819_Idle();
  BK4819_WriteRegister(BK4819_REG_30, 0xBFF1);
}

void BK4819_EnableTXLink(void) {
  BK4819_WriteRegister(
      BK4819_REG_30,
      BK4819_REG_30_ENABLE_VCO_CALIB | BK4819_REG_30_ENABLE_UNKNOWN |
          BK4819_REG_30_DISABLE_RX_LINK | BK4819_REG_30_ENABLE_AF_DAC |
          BK4819_REG_30_ENABLE_DISC_MODE | BK4819_REG_30_ENABLE_PLL_VCO |
          BK4819_REG_30_ENABLE_PA_GAIN | BK4819_REG_30_DISABLE_MIC_ADC |
          BK4819_REG_30_ENABLE_TX_DSP | BK4819_REG_30_DISABLE_RX_DSP);
}

void BK4819_PrepareTransmit(void) {
  BK4819_ExitBypass();
  BK4819_TxOn_Beep();
}

void BK4819_TxOn_Beep(void) {
  BK4819_WriteRegister(BK4819_REG_37, 0x9D1F);
  BK4819_WriteRegister(BK4819_REG_52, 0x028F);
  BK4819_Idle();
  BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
}

void BK4819_TurnsOffTones_TurnsOnRX(void) {
  BK4819_WriteRegister(BK4819_REG_70, 0);
  BK4819_SetAF(BK4819_AF_MUTE);
  BK4819_ExitTxMute();
  BK4819_Idle();
  BK4819_WriteRegister(
      BK4819_REG_30,
      BK4819_REG_30_ENABLE_VCO_CALIB | BK4819_REG_30_ENABLE_RX_LINK |
          BK4819_REG_30_ENABLE_AF_DAC | BK4819_REG_30_ENABLE_DISC_MODE |
          BK4819_REG_30_ENABLE_PLL_VCO | BK4819_REG_30_ENABLE_RX_DSP);
}

// ============================================================================
// Bypass
// ============================================================================

void BK4819_EnterBypass(void) {
  uint16_t reg = _ReadRegCached(BK4819_REG_7E);
  BK4819_WriteRegister(BK4819_REG_7E, reg & ~(0b111 << 3) & ~(0b111 << 0));
}

void BK4819_ExitBypass(void) {
  BK4819_SetAF(BK4819_AF_MUTE);
  BK4819_WriteRegister(BK4819_REG_7E, 0x302E);
}

// ============================================================================
// VOX
// ============================================================================

void BK4819_EnableVox(uint16_t enableThreshold, uint16_t disableThreshold) {
  uint16_t reg31 = BK4819_ReadRegister(BK4819_REG_31);
  BK4819_WriteRegister(BK4819_REG_46, 0xA000 | (enableThreshold & 0x07FF));
  BK4819_WriteRegister(BK4819_REG_79, 0x1800 | (disableThreshold & 0x07FF));
  BK4819_WriteRegister(BK4819_REG_7A, 0x289A); // 640ms disable delay
  BK4819_WriteRegister(BK4819_REG_31, reg31 | 4);
}

void BK4819_DisableVox(void) {
  BK4819_WriteRegister(BK4819_REG_31,
                       BK4819_ReadRegister(BK4819_REG_31) & 0xFFFB);
}

void BK4819_GetVoxAmp(uint16_t *result) {
  *result = BK4819_ReadRegister(BK4819_REG_64) & 0x7FFF;
}

// ============================================================================
// Scrambler
// ============================================================================

void BK4819_EnableScramble(uint8_t type) {
  BK4819_WriteRegister(BK4819_REG_31, BK4819_ReadRegister(BK4819_REG_31) | 2);
  BK4819_WriteRegister(BK4819_REG_71, (type * 0x0408) + 0x68DC);
  BK4819_WriteRegister(BK4819_REG_2B, BK4819_ReadRegister(BK4819_REG_2B) | 1);
}

void BK4819_DisableScramble(void) {
  BK4819_WriteRegister(BK4819_REG_31,
                       BK4819_ReadRegister(BK4819_REG_31) & 0xFFFD);
  BK4819_WriteRegister(BK4819_REG_2B,
                       0); // TODO: check if needed 0 only first bit
}

void BK4819_SetScrambler(uint8_t type) {
  if (type) {
    BK4819_EnableScramble(type);
  } else {
    BK4819_DisableScramble();
  }
}

// ============================================================================
// AFC
// ============================================================================

#define REG73_DISABLE (1 << 4)
#define REG73_LEVEL_MASK (0xF << 11)
#define REG73_LEVEL_DEF 7

void BK4819_SetAFC(uint8_t level) {
  if (level > 8)
    level = 8;
  uint16_t reg = _ReadRegCached(BK4819_REG_73);
  reg &= ~(REG73_LEVEL_MASK | REG73_DISABLE);
  reg |= (level == 0) ? ((REG73_LEVEL_DEF << 11) | REG73_DISABLE)
                      : ((8 - level) << 11);
  BK4819_WriteRegister(BK4819_REG_73, reg);
}

uint8_t BK4819_GetAFC(void) {
  uint16_t afc = BK4819_ReadRegister(BK4819_REG_73);
  return ((afc >> 4) & 1) ? 0 : 8 - ((afc >> 11) & 0b111);
}

void BK4819_SetAFCSpeed(uint8_t speed) {
  if (speed > 63)
    speed = 63;
  uint16_t reg = _ReadRegCached(BK4819_REG_73);
  reg = (reg & ~(63 << 5)) | ((63 - speed) << 5);
  BK4819_WriteRegister(BK4819_REG_73, reg);
}

uint8_t BK4819_GetAFCSpeed(void) {
  return 63 - ((BK4819_ReadRegister(BK4819_REG_73) >> 5) & 63);
}

// ============================================================================
// AF Response
// ============================================================================

// tx/rx, 3k/300Hz, gain -4..+4 dB (0 = default)
void BK4819_SetAFResponse(bool tx, bool is_3k, int8_t gain_db) {
  if (gain_db < -4)
    gain_db = -4;
  if (gain_db > 4)
    gain_db = 4;

  // index: gain_db+4 => 0=-4dB .. 4=0dB .. 8=+4dB
  static const uint16_t tbl_3k[9] = {
      0xDA00, 0xE800, 0xF200, 0xFA02, 0xF50B, 0xE61C, 0xDF22, 0xD42D, 0xCC35,
  };
  static const uint16_t tbl_300_d1[9] = {
      0x94A9, 0x935A, 0x920B, 0x91C1, 0x9009, 0x8F90, 0x8F46, 0x8ED8, 0x8D8F,
  };
  static const uint16_t tbl_300_d2[9] = {
      0x2EEE, 0x2EFF, 0x3010, 0x3040, 0x31A9, 0x31F3, 0x31E7, 0x3232, 0x3359,
  };

  const uint8_t idx = (uint8_t)(gain_db + 4);
  if (is_3k) {
    BK4819_WriteRegister(tx ? 0x74 : 0x75, tbl_3k[idx]);
  } else {
    BK4819_WriteRegister(tx ? 0x44 : 0x54, tbl_300_d1[idx]);
    BK4819_WriteRegister(tx ? 0x45 : 0x55, tbl_300_d2[idx]);
  }
}

// ============================================================================
// Frequency Scan
// ============================================================================

void BK4819_EnableFrequencyScan(void) {
  BK4819_WriteRegister(BK4819_REG_32, 0x0245);
}
void BK4819_EnableFrequencyScanEx(FreqScanTime t) {
  BK4819_WriteRegister(BK4819_REG_32, 0x0245 | (t << 14));
}
void BK4819_DisableFrequencyScan(void) {
  BK4819_WriteRegister(BK4819_REG_32, 0x0244);
}
void BK4819_StopScan(void) {
  BK4819_DisableFrequencyScan();
  BK4819_Idle();
}

void BK4819_EnableFrequencyScanEx2(FreqScanTime time, uint16_t hz) {
  BK4819_WriteRegister(BK4819_REG_32, (time << 14) | (hz << 1) | 1);
}

bool BK4819_GetFrequencyScanResult(uint32_t *frequency) {
  uint16_t high = BK4819_ReadRegister(BK4819_REG_0D);
  bool finished = (high & 0x8000) == 0;
  if (finished) {
    uint16_t low = BK4819_ReadRegister(BK4819_REG_0E);
    *frequency = (uint32_t)((high & 0x7FF) << 16) | low;
  }
  return finished;
}

void BK4819_SetScanFrequency(uint32_t frequency) {
  BK4819_SetFrequency(frequency);
  BK4819_WriteRegister(
      BK4819_REG_51,
      BK4819_REG_51_DISABLE_CxCSS | BK4819_REG_51_GPIO6_PIN2_NORMAL |
          BK4819_REG_51_TX_CDCSS_POSITIVE | BK4819_REG_51_MODE_CDCSS |
          BK4819_REG_51_CDCSS_23_BIT | BK4819_REG_51_1050HZ_NO_DETECTION |
          BK4819_REG_51_AUTO_CDCSS_BW_DISABLE |
          BK4819_REG_51_AUTO_CTCSS_BW_DISABLE);
  uint16_t reg30 = BK4819_ReadRegister(BK4819_REG_30);
  BK4819_WriteRegister(BK4819_REG_30, 0x200); // VCO calibrate
  SYSTICK_DelayUs(300);
  BK4819_WriteRegister(BK4819_REG_30, reg30);
  BK4819_RX_TurnOn();
}

// ============================================================================
// FSK
// ============================================================================

void BK4819_ResetFSK(void) {
  BK4819_WriteRegister(BK4819_REG_3F, 0x0000);
  BK4819_WriteRegister(BK4819_REG_59, 0x0068);
  SYSTICK_DelayMs(30);
  BK4819_Idle();
}

void BK4819_FskClearFifo(void) {
  BK4819_WriteRegister(BK4819_REG_59, (1u << 15) | (1u << 14) |
                                          BK4819_ReadRegister(BK4819_REG_59));
}

void BK4819_FskEnableRx(void) {
  BK4819_WriteRegister(BK4819_REG_59,
                       (1u << 12) | BK4819_ReadRegister(BK4819_REG_59));
}

void BK4819_FskEnableTx(void) {
  BK4819_WriteRegister(BK4819_REG_59,
                       (1u << 11) | BK4819_ReadRegister(BK4819_REG_59));
}

// ============================================================================
// Signal Measurements
// ============================================================================

uint8_t BK4819_GetLnaPeakRSSI(void) { return BK4819_ReadRegister(0x62) & 0xFF; }
uint8_t BK4819_GetAgcRSSI(void) {
  return (BK4819_ReadRegister(0x62) >> 8) & 0xFF;
}
uint8_t BK4819_GetGlitch(void) {
  return BK4819_ReadRegister(BK4819_REG_63) & 0xFF;
}
uint16_t BK4819_GetVoiceAmplitude(void) { return BK4819_ReadRegister(0x64); }
uint8_t BK4819_GetNoise(void) {
  return BK4819_ReadRegister(BK4819_REG_65) & 0x7F;
}
uint8_t BK4819_GetUpperChannelRelativePower(void) {
  return (BK4819_ReadRegister(0x66) >> 8) & 0xFF;
}
uint8_t BK4819_GetLowerChannelRelativePower(void) {
  return BK4819_ReadRegister(0x66) & 0xFF;
}
uint16_t BK4819_GetRSSI(void) {
  return BK4819_ReadRegister(BK4819_REG_67) & 0x1FF;
}
uint8_t BK4819_GetAfFreqOutNout(void) {
  return (BK4819_ReadRegister(0x6E) >> 9) & 0x7F;
}
uint8_t BK4819_GetAfFreqOutRout(void) {
  return BK4819_ReadRegister(0x6E) & 0x1FF;
}
uint8_t BK4819_GetAfTxRx(void) {
  return BK4819_ReadRegister(BK4819_REG_6F) & 0xFF;
}
int16_t BK4819_GetAFCValue(void) {
  return ((int16_t)BK4819_ReadRegister(0x6D) * 5) / 6;
}
uint8_t BK4819_GetSignalPower(void) {
  return (BK4819_ReadRegister(0x7E) >> 6) & 0x3F;
}
uint8_t BK4819_GetSNR(void) { return BK4819_ReadRegister(0x61) & 0xFF; }
