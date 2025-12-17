#ifndef _DRIVER_VCP_H
#define _DRIVER_VCP_H

#include "usb_config.h"
#include <stdint.h>
#include <string.h>

#define VCP_RX_BUF_SIZE 256

extern uint8_t VCP_RxBuf[VCP_RX_BUF_SIZE];
extern volatile uint32_t VCP_RxBufPointer;

void VCP_Init();

static inline void VCP_Send(const uint8_t *Buf, uint32_t Size) {
  cdc_acm_data_send_with_dtr(Buf, Size);
}

static inline void VCP_SendStr(const char *Str) {
  if (Str) {
    cdc_acm_data_send_with_dtr((const uint8_t *)Str, strlen(Str));
  }
}

static inline void VCP_SendAsync(const uint8_t *Buf, uint32_t Size) {
  cdc_acm_data_send_with_dtr_async(Buf, Size);
}

#endif // _DRIVER_VCP_H
