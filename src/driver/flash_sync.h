// flash_sync.h
#ifndef FLASH_SYNC_H
#define FLASH_SYNC_H

#include <stdbool.h>

// Простая блокировка для флеш-памяти
bool flash_lock(void);
void flash_unlock(void);
bool flash_is_locked(void);

#endif
