// flash_sync.c
#include "flash_sync.h"
#include "py32f071_ll_system.h"
#include "systick.h"

static volatile bool flash_busy = false;

bool flash_lock(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t start = Now();
  while (flash_busy) {
    if (Now() - start > 500) {
      flash_busy = false;
      __set_PRIMASK(primask);
      return false;
    }
    __enable_irq();
    for (volatile int i = 0; i < 1000; i++)
      __NOP();
    __disable_irq();
  }
  flash_busy = true;
  __set_PRIMASK(primask);
  return true;
}

void flash_unlock(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_busy = false;
  __set_PRIMASK(primask);
}

bool flash_is_locked(void) { return flash_busy; }
