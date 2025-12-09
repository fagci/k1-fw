#include <stdint.h>

extern uint32_t __bss_start__[];
extern uint32_t __bss_end__[];
extern uint8_t flash_data_start[];
extern uint8_t sram_data_start[];
extern uint8_t sram_data_end[];

void BSS_Init(void);
void DATA_Init(void);

void BSS_Init(void) {
  for (uint32_t *pBss = __bss_start__; pBss < __bss_end__; pBss++) {
    *pBss = 0;
  }
}

void DATA_Init(void) {
  volatile uint32_t *pDataRam = (volatile uint32_t *)sram_data_start;
  volatile uint32_t *pDataFlash = (volatile uint32_t *)flash_data_start;
  uint32_t Size = (uint32_t)sram_data_end - (uint32_t)sram_data_start;

  for (unsigned int i = 0; i < (Size / 4); i++) {
    *pDataRam++ = *pDataFlash++;
  }
}

