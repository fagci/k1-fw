#ifndef LFS_DRIVER_H
#define LFS_DRIVER_H

#include "../external/littlefs/lfs.h"
#include "py25q16.h"
#include <stdbool.h>
#include <stdint.h>

// Конфигурация LittleFS
#define LFS_BLOCK_SIZE 4096 // Размер стираемого блока (4KB)
#define LFS_BLOCK_COUNT 512 // Количество блоков (2MB / 4KB = 512)
#define LFS_READ_SIZE 256 // Рекомендуется как размер страницы флеш
#define LFS_PROG_SIZE 256  // Размер программирования
#define LFS_CACHE_SIZE 256 // Размер кеша
#define LFS_LOOKAHEAD_SIZE 32 // Для поиска свободных блоков

// Структура для LittleFS
typedef struct {
  // Конфигурация
  struct lfs_config config;

  // Статистика
  uint32_t read_count;
  uint32_t prog_count;
  uint32_t erase_count;
} lfs_storage_t;

// Функции инициализации
int fs_init();
bool lfs_file_exists(const char *path);

extern lfs_storage_t gStorage;
extern lfs_t gLfs;

#endif
