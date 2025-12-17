#include "vcp.h"
#include "py32f071_ll_bus.h"
#include "usb_config.h"

uint8_t VCP_RxBuf[VCP_RX_BUF_SIZE];
volatile uint32_t VCP_RxBufPointer = 0;

void VCP_Init() {
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA); // PA12:11
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USBD);

  cdc_acm_rx_buf_t rx_buf = {
      .buf = VCP_RxBuf,
      .size = sizeof(VCP_RxBuf),
      .write_pointer = &VCP_RxBufPointer,
  };
  cdc_acm_init(rx_buf);

  NVIC_SetPriority(USBD_IRQn, 3);
  NVIC_EnableIRQ(USBD_IRQn);
}
