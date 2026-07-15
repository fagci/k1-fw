#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../settings.h"
#include "gpio.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_spi.h"
#include "st7565.h"
#include "systick.h"

#define SPIx SPI1

#define PIN_CS GPIO_MAKE_PIN(GPIOB, LL_GPIO_PIN_2)
#define PIN_A0 GPIO_MAKE_PIN(GPIOA, LL_GPIO_PIN_6)

uint8_t gFrameBuffer[FRAME_LINES][LCD_WIDTH];
bool gLineChanged[FRAME_LINES]; // выставляется в graphics.c примитивами
bool gRedrawScreen = true;
bool gSuppressDisplayUpdates = false; // подавление обновлей дисплея

// ---------------------------------------------------------------------------
// SPI
// ---------------------------------------------------------------------------

static void SPI_Init(void) {
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SPI1);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

  LL_GPIO_InitTypeDef InitStruct;
  LL_GPIO_StructInit(&InitStruct);
  InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  InitStruct.Alternate = LL_GPIO_AF0_SPI1;
  InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;

  InitStruct.Pin = LL_GPIO_PIN_5;
  InitStruct.Pull = LL_GPIO_PULL_UP;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_MEDIUM;
  LL_GPIO_Init(GPIOA, &InitStruct);

  InitStruct.Pin = LL_GPIO_PIN_7;
  InitStruct.Pull = LL_GPIO_PULL_NO;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_MEDIUM;
  LL_GPIO_Init(GPIOA, &InitStruct);

  LL_SPI_InitTypeDef SPI_InitStruct;
  LL_SPI_StructInit(&SPI_InitStruct);
  SPI_InitStruct.TransferDirection = LL_SPI_FULL_DUPLEX;
  SPI_InitStruct.Mode = LL_SPI_MODE_MASTER;
  SPI_InitStruct.DataWidth = LL_SPI_DATAWIDTH_8BIT;
  SPI_InitStruct.ClockPolarity = LL_SPI_POLARITY_HIGH;
  SPI_InitStruct.ClockPhase = LL_SPI_PHASE_2EDGE;
  SPI_InitStruct.NSS = LL_SPI_NSS_SOFT;
  SPI_InitStruct.BitOrder = LL_SPI_MSB_FIRST;
  SPI_InitStruct.CRCCalculation = LL_SPI_CRCCALCULATION_DISABLE;
  SPI_InitStruct.BaudRate = LL_SPI_BAUDRATEPRESCALER_DIV16;
  LL_SPI_Init(SPIx, &SPI_InitStruct);

  LL_SPI_Enable(SPIx);
}

static inline void CS_Assert(void) { GPIO_ResetOutputPin(PIN_CS); }

static inline void CS_Release(void) {
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
  GPIO_SetOutputPin(PIN_CS);
}

static inline void A0_Set(void) { GPIO_SetOutputPin(PIN_A0); }
static inline void A0_Reset(void) { GPIO_ResetOutputPin(PIN_A0); }

static inline void SPI_WriteByte(uint8_t Value) {
  while (!LL_SPI_IsActiveFlag_TXE(SPIx))
    ;
  LL_SPI_TransmitData8(SPIx, Value);
  while (!LL_SPI_IsActiveFlag_RXNE(SPIx))
    ;
  (void)LL_SPI_ReceiveData8(SPIx); // Обязательно читаем ответный байт!
}

// ---------------------------------------------------------------------------
// Внутренняя отправка строки (CS должен быть уже выставлен)
// ---------------------------------------------------------------------------
static void FlushLine(uint8_t line) {
  A0_Reset();
  SPI_WriteByte(0xB0 | line);
  SPI_WriteByte(0x10 | ((4 >> 4) & 0x0F)); // column hi, offset 4
  SPI_WriteByte(0x00 | (4 & 0x0F));        // column lo
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
  A0_Set();
  const uint8_t *src = gFrameBuffer[line];
  for (uint8_t i = 0; i < LCD_WIDTH; i++)
    SPI_WriteByte(src[i]);
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
}

// ---------------------------------------------------------------------------
// Публичные функции вывода
// ---------------------------------------------------------------------------

void ST7565_SelectColumnAndLine(uint8_t Column, uint8_t Line) {
  A0_Reset();
  SPI_WriteByte(Line + 0xB0);
  SPI_WriteByte(((Column >> 4) & 0x0F) | 0x10);
  SPI_WriteByte(Column & 0x0F);
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
}

void ST7565_WriteByte(uint8_t Value) {
  A0_Reset();
  SPI_WriteByte(Value);
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
}

void ST7565_DrawLine(const unsigned int Column, const unsigned int Line,
                     const uint8_t *pBitmap, const unsigned int Size) {
  CS_Assert();
  ST7565_SelectColumnAndLine(Column + 4, Line);
  A0_Set();
  for (unsigned i = 0; i < Size; i++)
    SPI_WriteByte(pBitmap[i]);
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
  CS_Release();
}

void ST7565_MarkLineDirty(uint8_t line) {
  if (line < FRAME_LINES)
    gLineChanged[line] = true;
}

void ST7565_MarkRegionDirty(uint8_t start_line, uint8_t end_line) {
  for (uint8_t l = start_line; l <= end_line && l < FRAME_LINES; l++)
    gLineChanged[l] = true;
}

void ST7565_ForceFullRedraw(void) {
  memset(gLineChanged, true, sizeof(gLineChanged));
  gRedrawScreen = true;
}

// ---------------------------------------------------------------------------
// Blit — O(FRAME_LINES), никакого memcmp / checksum
// ---------------------------------------------------------------------------
void ST7565_Blit(void) {
  bool any = false;
  for (uint8_t l = 0; l < FRAME_LINES; l++) {
    if (gLineChanged[l]) {
      any = true;
      break;
    }
  }
  if (!any) {
    gRedrawScreen = false;
    return;
  }

  CS_Assert();
  for (uint8_t line = 0; line < FRAME_LINES; line++) {
    if (gLineChanged[line]) {
      FlushLine(line);
      gLineChanged[line] = false;
    }
  }
  CS_Release();
  gRedrawScreen = false;
}

void ST7565_BlitLine(unsigned line) {
  if (line >= FRAME_LINES || !gLineChanged[line])
    return;
  CS_Assert();
  FlushLine(line);
  CS_Release();
  gLineChanged[line] = false;
}

void ST7565_FillScreen(uint8_t value) {
  memset(gFrameBuffer, value, sizeof(gFrameBuffer));
  memset(gLineChanged, true, sizeof(gLineChanged));
  gRedrawScreen = true;
}

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------

#define ST7565_CMD_SOFTWARE_RESET 0xE2
#define ST7565_CMD_BIAS_SELECT 0xA2
#define ST7565_CMD_COM_DIRECTION 0xC0
#define ST7565_CMD_SEG_DIRECTION 0xA0
#define ST7565_CMD_INVERSE_DISPLAY 0xA6
#define ST7565_CMD_ALL_PIXEL_ON 0xA4
#define ST7565_CMD_REGULATION_RATIO 0x20
#define ST7565_CMD_SET_EV 0x81
#define ST7565_CMD_POWER_CIRCUIT 0x28
#define ST7565_CMD_SET_START_LINE 0x40
#define ST7565_CMD_DISPLAY_ON_OFF 0xAE

static const uint8_t init_cmds[] = {
    ST7565_CMD_BIAS_SELECT | 0,   ST7565_CMD_COM_DIRECTION | (0 << 3),
    ST7565_CMD_SEG_DIRECTION | 1, ST7565_CMD_INVERSE_DISPLAY | 0,
    ST7565_CMD_ALL_PIXEL_ON | 0,  ST7565_CMD_REGULATION_RATIO | 4,
};

static void send_init_cmds(void) {
  for (uint8_t i = 0; i < sizeof(init_cmds); i++)
    ST7565_WriteByte(init_cmds[i]);
  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + gSettings.contrast);
}

void ST7565_Init(void) {
  SPI_Init();

  CS_Assert();
  ST7565_WriteByte(ST7565_CMD_SOFTWARE_RESET);
  SYSTICK_DelayMs(120); // Критично: дисплею нужно время на сброс

  send_init_cmds();

  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b011);
  SYSTICK_DelayMs(1);
  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b110);
  SYSTICK_DelayMs(1);
  for (uint8_t i = 0; i < 4; i++)
    ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b111);
  SYSTICK_DelayMs(40); // Критично: время на стабилизацию напряжения

  ST7565_WriteByte(ST7565_CMD_SET_START_LINE | 0);
  ST7565_WriteByte(ST7565_CMD_DISPLAY_ON_OFF | 1);
  CS_Release();

  memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
  memset(gLineChanged, true, sizeof(gLineChanged));
  gRedrawScreen = true;
}

void ST7565_SetContrast(uint8_t contrast) {
  CS_Assert();
  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + contrast);
  CS_Release();
}

void ST7565_FixInterfGlitch(void) {
  CS_Assert();
  send_init_cmds();
  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b111);
  ST7565_WriteByte(ST7565_CMD_SET_START_LINE | 0);
  ST7565_WriteByte(ST7565_CMD_DISPLAY_ON_OFF | 1);
  CS_Release();

  memset(gLineChanged, true, sizeof(gLineChanged));
  gRedrawScreen = true;
}
