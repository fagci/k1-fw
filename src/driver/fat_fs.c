#include "fat_fs.h"
#include "../external/printf/printf.h"
#include "py25q16.h"
#include <string.h>

// Внешние функции для работы с флеш

static bool fs_formatted = false;

// Создание Boot Sector
static void FAT_CreateBootSector(fat_boot_sector_t *bs) {
  memset(bs, 0, sizeof(fat_boot_sector_t));

  // Jump instruction
  bs->jmp_boot[0] = 0xEB;
  bs->jmp_boot[1] = 0x3C;
  bs->jmp_boot[2] = 0x90;

  // OEM Name
  memcpy(bs->oem_name, "MSWIN4.1", 8);

  // BPB
  bs->bytes_per_sector = FAT_SECTOR_SIZE;
  bs->sectors_per_cluster = FAT_SECTORS_PER_CLUSTER;
  bs->reserved_sectors = FAT_RESERVED_SECTORS;
  bs->num_fats = FAT_NUM_FATS;
  bs->root_entry_count = FAT_ROOT_ENTRIES;
  bs->total_sectors_16 = FAT_TOTAL_SECTORS;
  bs->media_type = 0xF0; // Change to 0xF0 for removable media (USB flash)
  bs->fat_size_16 = FAT_SECTORS_PER_FAT;
  bs->sectors_per_track = 63;
  bs->num_heads = 255;
  bs->hidden_sectors = 0;
  bs->total_sectors_32 = 0;

  // Extended BPB
  bs->drive_number = 0x80;
  bs->reserved1 = 0;
  bs->boot_signature = 0x29;
  bs->volume_id = 0x12345678;
  memcpy(bs->volume_label, "PY32 FLASH ", 11);
  memcpy(bs->fs_type, "FAT12   ", 8); // Change to FAT12 (matches cluster count)

  // Boot signature
  bs->signature = 0xAA55;
}

// Создание пустой FAT таблицы
static void FAT_CreateFATTable(uint8_t *buf) {
  memset(buf, 0, FAT_SECTOR_SIZE);

  // First two entries reserved
  uint16_t *fat = (uint16_t *)buf;
  fat[0] = 0xFFF0; // Change to match media 0xF0 (was 0xFFF8)
  fat[1] = 0xFFFF; // End of chain for first cluster
}

// Создание Root Directory с volume label
static void FAT_CreateRootDir(uint8_t *buf) {
  memset(buf, 0, FAT_SECTOR_SIZE);

  fat_dir_entry_t *entry = (fat_dir_entry_t *)buf;

  // Volume label entry
  memcpy(entry->name, "PY32 FLASH ", 11);
  entry->attr = FAT_ATTR_VOLUME_ID;
  entry->nt_res = 0;
  entry->crt_time_tenth = 0;
  entry->crt_time = 0;
  entry->crt_date = 0x4E21; // 2019-01-01
  entry->lst_acc_date = 0x4E21;
  entry->fst_clus_hi = 0;
  entry->wrt_time = 0;
  entry->wrt_date = 0x4E21;
  entry->fst_clus_lo = 0;
  entry->file_size = 0;
}

void FAT_Init(void) {
  PY25Q16_Init();
  printf("Flash init done\n");

  // Сигнатура находится по offset 510-511 (0x1FE-0x1FF)
  uint8_t sig[2];
  PY25Q16_ReadBuffer(510, sig, 2);

  printf("Boot sig: %02X %02X\n", sig[0], sig[1]);

  if (sig[0] == 0x55 && sig[1] == 0xAA) {
    fs_formatted = true;
    printf("FS already formatted\n");
  } else {
    printf("Formatting flash...\n");
    FAT_Format();
    printf("Format complete\n");
    fs_formatted = true;
  }
}

/* void FAT_Init(void) {
  PY25Q16_Init();

  // Быстрая проверка: читаем только сигнатуру boot sector
  uint8_t sig[2];
  PY25Q16_ReadBuffer(510, sig, 2); // Сигнатура в конце boot sector

  if (sig[0] == 0x55 && sig[1] == 0xAA) {
    // Boot sector существует, не форматируем
    fs_formatted = true;
  } else {
    // Нужно форматирование
    FAT_Format();
    fs_formatted = true;
  }
} */

void FAT_Format(void) {
  uint8_t buf[FAT_SECTOR_SIZE];

  // 1. Erase first 32KB (covers boot + 2 FATs + root)
  for (uint32_t addr = 0; addr < 32768; addr += 4096) {
    PY25Q16_SectorErase(addr);
  }

  // 2. Create and write Boot Sector
  fat_boot_sector_t bs;
  FAT_CreateBootSector(&bs);
  PY25Q16_WriteBuffer(0, &bs, sizeof(fat_boot_sector_t));

  // 3. Create and write FAT tables (now 2, identical)
  FAT_CreateFATTable(buf);

  for (int fat_num = 0; fat_num < FAT_NUM_FATS; fat_num++) {
    uint32_t fat_addr =
        (FAT_TABLE_SECTOR + fat_num * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
    PY25Q16_WriteBuffer(fat_addr, buf, FAT_SECTOR_SIZE);

    // Remaining FAT sectors = zeros
    memset(buf, 0, FAT_SECTOR_SIZE);
    for (int i = 1; i < FAT_SECTORS_PER_FAT; i++) {
      uint32_t addr = (FAT_TABLE_SECTOR + fat_num * FAT_SECTORS_PER_FAT + i) *
                      FAT_SECTOR_SIZE;
      PY25Q16_WriteBuffer(addr, buf, FAT_SECTOR_SIZE);
    }
  }

  // 4. Create and write Root Directory
  FAT_CreateRootDir(buf);
  uint32_t root_addr = FAT_ROOT_DIR_SECTOR * FAT_SECTOR_SIZE;
  PY25Q16_WriteBuffer(root_addr, buf, FAT_SECTOR_SIZE);

  // Remaining root sectors = zeros
  memset(buf, 0, FAT_SECTOR_SIZE);
  for (int i = 1; i < FAT_ROOT_DIR_SECTORS; i++) {
    uint32_t addr = (FAT_ROOT_DIR_SECTOR + i) * FAT_SECTOR_SIZE;
    PY25Q16_WriteBuffer(addr, buf, FAT_SECTOR_SIZE);
  }

  fs_formatted = true;
}

void FAT_ReadBlock(uint32_t lba, uint8_t *buf) {
  uint32_t addr = lba * FAT_SECTOR_SIZE;
  PY25Q16_ReadBuffer(addr, buf, FAT_SECTOR_SIZE);
}

void FAT_WriteBlock(uint32_t lba, const uint8_t *buf) {
  uint32_t addr = lba * FAT_SECTOR_SIZE;

  // Если адрес выровнен на границу сектора флеш (4KB), стираем сектор
  /* if ((addr % 4096) == 0) {
    PY25Q16_SectorErase(addr);
  } */

  PY25Q16_WriteBuffer(addr, buf, FAT_SECTOR_SIZE);
}
