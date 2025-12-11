#ifndef DRIVER_SYSTICK_H
#define DRIVER_SYSTICK_H

#include <stdint.h>

void SYSTICK_Init(void);
void SYSTICK_DelayUs(uint32_t Delay);
void SYSTICK_DelayMs(uint32_t Delay);
uint32_t Now();

#endif
