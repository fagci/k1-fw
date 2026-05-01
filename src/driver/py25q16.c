#include "py25q16.h"
#include "../external/printf/printf.h"
#include "flash_sync.h"
#include "gpio.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_spi.h"
#include "py32f071_ll_system.h"
#include "systick.h"

// #define DEBUG

#define SPIx SPI2
#define CHANNEL_RD LL_DMA_CHANNEL_4
#define CHANNEL_WR LL_DMA_CHANNEL_5

#define CS_PIN GPIO_MAKE_PIN(GPIOA, LL_GPIO_PIN_3)

#define SECTOR_SIZE 4096
#define PAGE_SIZE 0x100

bool gEepromWrite = false;

static uint32_t SectorCacheAddr = 0x1000000;
static uint8_t SectorCache[SECTOR_SIZE];
static uint8_t BlackHole[1];
static volatile bool TC_Flag;

static uint32_t last_operation_time = 0;
static uint32_t operation_count = 0;

static inline void CS_Assert() { GPIO_ResetOutputPin(CS_PIN); }

static inline void CS_Release() { GPIO_SetOutputPin(CS_PIN); }

static void SPI_Init() {
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

  do {
    // SCK: PA0
    // MOSI: PA1
    // MISO: PA2

    LL_GPIO_InitTypeDef InitStruct;
    LL_GPIO_StructInit(&InitStruct);
    InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    // MEDIUM вместо VERY_HIGH — снижаем RF помехи от SPI2 flash
    InitStruct.Speed = LL_GPIO_SPEED_FREQ_MEDIUM;
    InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    InitStruct.Pull = LL_GPIO_PULL_UP;

    InitStruct.Pin = LL_GPIO_PIN_0;
    InitStruct.Alternate = LL_GPIO_AF8_SPI2;
    LL_GPIO_Init(GPIOA, &InitStruct);

    InitStruct.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_2;
    InitStruct.Alternate = LL_GPIO_AF9_SPI2;
    LL_GPIO_Init(GPIOA, &InitStruct);

  } while (0);

  LL_SYSCFG_SetDMARemap(DMA1, CHANNEL_RD, LL_SYSCFG_DMA_MAP_SPI2_RD);
  LL_SYSCFG_SetDMARemap(DMA1, CHANNEL_WR, LL_SYSCFG_DMA_MAP_SPI2_WR);

  NVIC_SetPriority(DMA1_Channel4_5_6_7_IRQn, 1);
  NVIC_EnableIRQ(DMA1_Channel4_5_6_7_IRQn);

  LL_SPI_InitTypeDef InitStruct;
  LL_SPI_StructInit(&InitStruct);
  InitStruct.Mode = LL_SPI_MODE_MASTER;
  InitStruct.TransferDirection = LL_SPI_FULL_DUPLEX;
  InitStruct.ClockPhase = LL_SPI_PHASE_2EDGE;
  InitStruct.ClockPolarity = LL_SPI_POLARITY_HIGH;
  // DIV8 вместо DIV2 — снижаем RF помехи (24 MHz → 6 MHz)
  InitStruct.BaudRate = LL_SPI_BAUDRATEPRESCALER_DIV16;
  InitStruct.BitOrder = LL_SPI_MSB_FIRST;
  InitStruct.NSS = LL_SPI_NSS_SOFT;
  InitStruct.CRCCalculation = LL_SPI_CRCCALCULATION_DISABLE;
  LL_SPI_Init(SPIx, &InitStruct);

  LL_SPI_Enable(SPIx);
}

static void SPI_ReadBuf(uint8_t *Buf, uint32_t Size) {
  LL_SPI_Disable(SPIx);
  LL_DMA_DisableChannel(DMA1, CHANNEL_RD);
  LL_DMA_DisableChannel(DMA1, CHANNEL_WR);

  LL_DMA_ClearFlag_GI4(DMA1);
  LL_DMA_ClearFlag_TC4(DMA1);
  LL_DMA_ClearFlag_TE4(DMA1);

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_RD,                 //
                        LL_DMA_DIRECTION_PERIPH_TO_MEMORY //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_INCREMENT     //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_HIGH        //
  );

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_WR,                 //
                        LL_DMA_DIRECTION_MEMORY_TO_PERIPH //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_NOINCREMENT   //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_HIGH        //
  );

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_RD, (uint32_t)Buf);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_RD, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_RD, Size);

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_WR, (uint32_t)BlackHole);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_WR, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_WR, Size);

  TC_Flag = false;
  LL_DMA_EnableIT_TC(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_WR);

  LL_SPI_EnableDMAReq_RX(SPIx);
  LL_SPI_Enable(SPIx);
  LL_SPI_EnableDMAReq_TX(SPIx);
}

static void SPI_WriteBuf(const uint8_t *Buf, uint32_t Size) {
  gEepromWrite = true;
  LL_SPI_Disable(SPIx);
  LL_DMA_DisableChannel(DMA1, CHANNEL_RD);
  LL_DMA_DisableChannel(DMA1, CHANNEL_WR);

  LL_DMA_ClearFlag_GI4(DMA1);
  LL_DMA_ClearFlag_TC4(DMA1);
  LL_DMA_ClearFlag_TE4(DMA1);

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_RD,                 //
                        LL_DMA_DIRECTION_PERIPH_TO_MEMORY //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_NOINCREMENT   //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_HIGH        //
  );

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_WR,                 //
                        LL_DMA_DIRECTION_MEMORY_TO_PERIPH //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_INCREMENT     //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_HIGH        //
  );

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_RD, (uint32_t)BlackHole);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_RD, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_RD, Size);

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_WR, (uint32_t)Buf);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_WR, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_WR, Size);

  TC_Flag = false;
  LL_DMA_EnableIT_TC(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_WR);

  LL_SPI_EnableDMAReq_RX(SPIx);
  LL_SPI_Enable(SPIx);
  LL_SPI_EnableDMAReq_TX(SPIx);
}

// ОЖИДАНИЕ DMA с таймаутом и защитой
static bool wait_for_dma_complete(uint32_t max_wait_ms) {
  uint32_t start = Now();

  while (!TC_Flag) {
    if (Now() - start > max_wait_ms) {
      // Таймаут - останавливаем DMA
      LL_DMA_DisableChannel(DMA1, CHANNEL_RD);
      LL_DMA_DisableChannel(DMA1, CHANNEL_WR);
      LL_SPI_DisableDMAReq_TX(SPIx);
      LL_SPI_DisableDMAReq_RX(SPIx);
      return false;
    }
    __WFI(); // Засыпаем до прерывания DMA — экономим CPU
  }
  return true;
}

static __attribute__((noinline)) uint8_t SPI_WriteByte(uint8_t Value) {
  uint32_t start = Now();
  while (!LL_SPI_IsActiveFlag_TXE(SPIx)) {
    if (Now() - start > 100) {
      CS_Release();
      return 0xFF;
    }
  }
  LL_SPI_TransmitData8(SPIx, Value);
  start = Now();
  while (!LL_SPI_IsActiveFlag_RXNE(SPIx)) {
    if (Now() - start > 100) {
      CS_Release();
      return 0xFF;
    }
  }
  return LL_SPI_ReceiveData8(SPIx);
}

static void WriteAddr(uint32_t Addr) {
  SPI_WriteByte(0xff & (Addr >> 16));
  SPI_WriteByte(0xff & (Addr >> 8));
  SPI_WriteByte(0xff & Addr);
}

static uint8_t ReadStatusReg(uint32_t Which) {
  uint8_t Cmd;
  switch (Which) {
  case 0:
    Cmd = 0x5;
    break;
  case 1:
    Cmd = 0x35;
    break;
  case 2:
    Cmd = 0x15;
    break;
  default:
    return 0;
  }

  CS_Assert();
  SPI_WriteByte(Cmd);
  uint8_t Value = SPI_WriteByte(0xff);
  CS_Release();

  return Value;
}

// УПРОЩЕННАЯ функция WaitWIP
static bool WaitWIP(uint32_t timeout_ms) {
  uint32_t start = Now();
  while (1) {
    if ((ReadStatusReg(0) & 0x01) == 0)
      return true;
    if (Now() - start > timeout_ms)
      return false;
    SYSTICK_DelayMs(1); // или __WFI() если SysTick тикает каждую мс
  }
}

static void WriteEnable(void) {
  CS_Assert();
  SPI_WriteByte(0x06);
  CS_Release();

  // Короткая задержка после команды
  for (volatile int i = 0; i < 50; i++) {
    __NOP();
  }
}

// УПРОЩЕННАЯ PageProgram без DMA (только байтовая запись)
static bool PageProgram(uint32_t Addr, const uint8_t *Buf, uint32_t Size) {
  if (Size > PAGE_SIZE)
    Size = PAGE_SIZE;
  if (Size == 0)
    return true;

  WriteEnable();

  CS_Assert();
  SPI_WriteByte(0x02);
  WriteAddr(Addr);

  SPI_WriteBuf(Buf, Size);
  if (!wait_for_dma_complete(50)) {
    CS_Release();
    return false;
  }

  CS_Release();
  return WaitWIP(100);
}

void PY25Q16_Init() {
  CS_Release();
  SPI_Init();
}

// py25q16.c - оптимизированное чтение
void PY25Q16_ReadBuffer(uint32_t Address, void *pBuffer, uint32_t Size) {
  CS_Assert();

  // Быстрое чтение с dummy byte (стандарт)
  SPI_WriteByte(0x0B); // Fast Read
  SPI_WriteByte(Address >> 16);
  SPI_WriteByte(Address >> 8);
  SPI_WriteByte(Address);
  SPI_WriteByte(0xFF); // Dummy byte

  // Используем DMA для чтения, если размер большой
  if (Size > 64) {
    SPI_ReadBuf(pBuffer, Size);
    if (!wait_for_dma_complete(100)) {
      // Fallback на байтовое чтение при ошибке
      for (uint32_t i = 0; i < Size; i++) {
        ((uint8_t *)pBuffer)[i] = SPI_WriteByte(0xFF);
      }
    }
  } else {
    // Для малых блоков - байтовое чтение
    for (uint32_t i = 0; i < Size; i++) {
      ((uint8_t *)pBuffer)[i] = SPI_WriteByte(0xFF);
    }
  }

  CS_Release();
}
/* void PY25Q16_ReadBuffer(uint32_t Address, void *pBuffer, uint32_t Size) {
  CS_Assert();

  // Команда быстрого чтения с dummy byte
  SPI_WriteByte(0x0B); // Fast Read
  SPI_WriteByte(Address >> 16);
  SPI_WriteByte(Address >> 8);
  SPI_WriteByte(Address);
  SPI_WriteByte(0xFF); // Dummy byte для fast read

  // Чтение данных
  for (uint32_t i = 0; i < Size; i++) {
    ((uint8_t *)pBuffer)[i] = SPI_WriteByte(0xFF);
  }

  CS_Release();
} */

void PY25Q16_WriteBuffer(uint32_t Address, const void *pBuffer, uint32_t Size,
                         bool Append) {
  if (!flash_lock())
    return;
#ifdef DEBUG
  printf("WriteBuffer: 0x%06lx, %lu bytes\n", Address, Size);
#endif

  // Защита от слишком частых операций
  uint32_t now = Now();
  if (now - last_operation_time < 20) { // Минимум 20мс между операциями
    uint32_t delay = 20 - (now - last_operation_time);
    SYSTICK_DelayMs(delay);
  }

  const uint8_t *ptr = (const uint8_t *)pBuffer;
  uint32_t written = 0;

  while (written < Size) {
    uint32_t page_addr = Address + written;
    uint32_t page_offset = page_addr % PAGE_SIZE;
    uint32_t to_write = PAGE_SIZE - page_offset;

    if (to_write > Size - written) {
      to_write = Size - written;
    }

    if (!PageProgram(page_addr, ptr + written, to_write)) {
#ifdef DEBUG
      printf("PageProgram failed at 0x%06lx\n", page_addr);
#endif
      break;
    }

    written += to_write;

    // Небольшая задержка между страницами
    /* if (written < Size) {
      for (volatile int i = 0; i < 1000; i++) {
        __NOP();
      }
    } */
  }

  last_operation_time = Now();

  flash_unlock();
}

void PY25Q16_SectorErase(uint32_t Address) {
  if (!flash_lock())
    return;
  Address &= ~(SECTOR_SIZE - 1);

#ifdef DEBUG
  printf("SectorErase: 0x%06lx\n", Address);
#endif

  // Защита от слишком частых стираний
  uint32_t now = Now();
  if (now - last_operation_time < 100) { // Минимум 100мс между операциями
    uint32_t delay = 100 - (now - last_operation_time);
    SYSTICK_DelayMs(delay);
  }

  operation_count++;

  // Выполняем стирание
  WriteEnable();
  gEepromWrite = true;

  CS_Assert();
  SPI_WriteByte(0x20); // Sector Erase (4KB)
  WriteAddr(Address);
  CS_Release();

  WaitWIP(500); // 500ms таймаут для стирания

  last_operation_time = Now();
  flash_unlock();
}

void PY25Q16_FullErase() {
  WriteEnable();
  CS_Assert();
  SPI_WriteByte(0xC7); // Можно также использовать 0x60
  CS_Release();
  WaitWIP(60000);
}

void DMA1_Channel4_5_6_7_IRQHandler() {
  if (LL_DMA_IsActiveFlag_TC4(DMA1) &&
      LL_DMA_IsEnabledIT_TC(DMA1, CHANNEL_RD)) {
    LL_DMA_DisableIT_TC(DMA1, CHANNEL_RD);
    LL_DMA_ClearFlag_TC4(DMA1);

    // Очищаем флаги SPI
    while (LL_SPI_TX_FIFO_EMPTY != LL_SPI_GetTxFIFOLevel(SPIx))
      ;
    while (LL_SPI_IsActiveFlag_BSY(SPIx))
      ;
    while (LL_SPI_RX_FIFO_EMPTY != LL_SPI_GetRxFIFOLevel(SPIx))
      ;

    LL_SPI_DisableDMAReq_TX(SPIx);
    LL_SPI_DisableDMAReq_RX(SPIx);

    TC_Flag = true;
  }
}

// Простой тест записи/чтения
bool test_flash_simple(void) {
  printf("\n=== Flash Simple Test ===\n");

  // Тест 1: Запись и чтение 1024 байт
  printf("Test 1KB write/read...\n");

  uint8_t write_data[1024];
  uint8_t read_data[1024];

  // Заполняем тестовыми данными
  for (int i = 0; i < 1024; i++) {
    write_data[i] = i % 256;
  }

  // Стираем сектор
  printf("  Erasing sector at 0x00A000...\n");
  PY25Q16_SectorErase(0x00A000);

  // Записываем
  printf("  Writing 1024 bytes...\n");
  uint32_t start = Now();
  PY25Q16_WriteBuffer(0x00A000, write_data, 1024, false);
  uint32_t write_time = Now() - start;
  printf("  Write time: %lu ms\n", write_time);

  // Читаем обратно
  printf("  Reading back...\n");
  start = Now();
  PY25Q16_ReadBuffer(0x00A000, read_data, 1024);
  uint32_t read_time = Now() - start;
  printf("  Read time: %lu ms\n", read_time);

  // Проверяем
  bool ok = true;
  for (int i = 0; i < 1024; i++) {
    if (write_data[i] != read_data[i]) {
      printf("  Mismatch at %d: 0x%02X != 0x%02X\n", i, write_data[i],
             read_data[i]);
      ok = false;
      break;
    }
  }

  printf("  Result: %s\n", ok ? "PASS" : "FAIL");
  printf("=== Test Complete ===\n\n");

  return ok;
}
