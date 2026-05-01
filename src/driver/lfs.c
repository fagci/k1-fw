#include "lfs.h"
#include "../external/printf/printf.h"
#include <string.h>

// Глобальные переменные
lfs_storage_t gStorage;
lfs_t gLfs;

// Глобальные буферы
uint8_t lfs_read_buffer[LFS_CACHE_SIZE];
uint8_t lfs_prog_buffer[LFS_CACHE_SIZE];
uint8_t lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE / 8];

// Чтение блока
static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size) {
  uint32_t addr = block * c->block_size + off;
  PY25Q16_ReadBuffer(addr, buffer, size);
  gStorage.read_count++;
  return 0;
}

// Программирование блока
// lfs гарантирует, что блок стёрт перед вызовом prog (через lfs_erase).
// Для NOR flash/SPI flash prog только сбрасывает биты 1→0.
static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, const void *buffer, lfs_size_t size) {
  uint32_t addr = block * c->block_size + off;
  PY25Q16_WriteBuffer(addr, (uint8_t *)buffer, size, true);
  gStorage.prog_count++;
  return 0;
}

// Стирание блока
static int lfs_erase(const struct lfs_config *c, lfs_block_t block) {
  PY25Q16_SectorErase(block * c->block_size);
  gStorage.erase_count++;
  return 0;
}

// Синхронизация
static int lfs_sync(const struct lfs_config *c) {
  // SPI флеш не требует синхронизации
  return 0;
}

int lfs_storage_init(lfs_storage_t *storage) {
  memset(storage, 0, sizeof(lfs_storage_t));

  storage->config.context = NULL;
  storage->config.read = lfs_read;
  storage->config.prog = lfs_prog;
  storage->config.erase = lfs_erase;
  storage->config.sync = lfs_sync;
  storage->config.read_size = LFS_READ_SIZE;
  storage->config.prog_size = LFS_PROG_SIZE;
  storage->config.block_size = LFS_BLOCK_SIZE;
  storage->config.block_count = LFS_BLOCK_COUNT;
  storage->config.cache_size = LFS_CACHE_SIZE;
  storage->config.lookahead_size = LFS_LOOKAHEAD_SIZE;
  storage->config.block_cycles = 500;
  storage->config.read_buffer = lfs_read_buffer;
  storage->config.prog_buffer = lfs_prog_buffer;
  storage->config.lookahead_buffer = lfs_lookahead_buffer;

  return 0;
}

int fs_format(lfs_storage_t *storage) {
  int err = lfs_format(&gLfs, &storage->config);
  if (err) {
    printf("Format error: %d\n", err);
    return err;
  }
  printf("LittleFS formatted successfully\n");
  return 0;
}

int fs_mount(lfs_storage_t *storage, lfs_t *lfs) {
  int err = lfs_mount(lfs, &storage->config);
  if (err) {
    printf("Mount error: %d, trying to format...\n", err);
    err = lfs_format(lfs, &storage->config);
    if (err)
      return err;
    err = lfs_mount(lfs, &storage->config);
  }
  return err;
}

// === HIGH-LEVEL API (аналогичный FAT API) ===

int fs_init(void) {
  printf("[LFS] Initializing LittleFS\n");

  // Инициализируем хранилище
  lfs_storage_init(&gStorage);

  // Монтируем файловую систему
  int err = fs_mount(&gStorage, &gLfs);
  if (err) {
    printf("[LFS] Failed to mount: %d\n", err);
    return -1;
  }

  printf("[LFS] Mounted successfully\n");

  return 0;
}

uint32_t fs_get_free_space(void) {
  lfs_ssize_t free = lfs_fs_size(&gLfs);
  if (free < 0)
    return 0;

  // Вычисляем свободное пространство
  uint32_t total_blocks = LFS_BLOCK_COUNT;
  uint32_t used_blocks = free;
  uint32_t free_blocks = total_blocks - used_blocks;

  return free_blocks * LFS_BLOCK_SIZE;
}

bool lfs_file_exists(const char *path) {
  struct lfs_info info;
  return lfs_stat(&gLfs, path, &info) == 0;
}
