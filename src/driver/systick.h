#ifndef DRIVER_SYSTICK_H
#define DRIVER_SYSTICK_H

#include <stdint.h>

void SYSTICK_Init(void);
void SYSTICK_DelayUs(uint32_t Delay);

#endif
