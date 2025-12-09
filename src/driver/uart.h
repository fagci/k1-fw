#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include <stdbool.h>
#include <stdint.h>

extern uint8_t UART_DMA_Buffer[256];

void UART_Init(void);
void UART_Send(const void *pBuffer, uint32_t Size);
void UART_LogSend(const void *pBuffer, uint32_t Size);

#endif
