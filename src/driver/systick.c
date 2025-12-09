#include "systick.h"
#include "py32f0xx.h"

static uint32_t gTickMultiplier;

void SYSTICK_Init(void) {
  SysTick_Config(480000);
  gTickMultiplier = 48;

  NVIC_SetPriority(SysTick_IRQn, 0);
}

void SYSTICK_DelayUs(uint32_t Delay) {
  const uint32_t ticks = Delay * gTickMultiplier;
  uint32_t elapsed_ticks = 0;
  uint32_t Start = SysTick->LOAD;
  uint32_t Previous = SysTick->VAL;
  do {
    uint32_t Current;

    do {
      Current = SysTick->VAL;
    } while (Current == Previous);

    uint32_t Delta = ((Current < Previous) ? -Current : Start - Current);

    elapsed_ticks += Delta + Previous;

    Previous = Current;
  } while (elapsed_ticks < ticks);
}

void SysTick_Handler(void) {}
