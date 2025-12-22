#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "system.h"
#include <stdbool.h>

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  printf("Hawk\n");

  GPIO_EnableAudioPath();

  SYS_Main();
}
