#include "fat_fs.h"
#include <string.h>

// Внешние функции для работы с флеш
extern void PY25Q16_Init(void);
extern void PY25Q16_ReadBuffer(uint32_t Address, void *pBuffer, uint32_t Size);
extern void PY25Q16_WriteBuffer(uint32_t Address, const void *pBuffer, uint32_t Size, bool Append);
extern void PY25Q16_SectorErase(uint32_t Address);

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
    
    // BPB (BIOS Parameter Block)
    bs->bytes_per_sector = FAT_SECTOR_SIZE;
    bs->sectors_per_cluster = FAT_SECTORS_PER_CLUSTER;
    bs->reserved_sectors = FAT_RESERVED_SECTORS;
    bs->num_fats = FAT_NUM_FATS;
    bs->root_entry_count = FAT_ROOT_ENTRIES;
    bs->total_sectors_16 = FAT_TOTAL_SECTORS;
    bs->media_type = 0xF8;  // Fixed disk
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
    memcpy(bs->fs_type, "FAT16   ", 8);
    
    // Boot signature
    bs->signature = 0xAA55;
}

// Создание пустой FAT таблицы
static void FAT_CreateFATTable(uint8_t *buf) {
    memset(buf, 0, FAT_SECTOR_SIZE);
    
    // Первые два entry зарезервированы
    uint16_t *fat = (uint16_t *)buf;
    fat[0] = 0xFFF8;  // Media descriptor
    fat[1] = 0xFFFF;  // End of chain для первого кластера
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
    entry->crt_date = 0x4E21;  // 2019-01-01
    entry->lst_acc_date = 0x4E21;
    entry->fst_clus_hi = 0;
    entry->wrt_time = 0;
    entry->wrt_date = 0x4E21;
    entry->fst_clus_lo = 0;
    entry->file_size = 0;
}

void FAT_Init(void) {
    PY25Q16_Init();
    
    // Небольшая задержка для инициализации флеш
    for(volatile int i = 0; i < 100000; i++);
    
    // Проверяем, отформатирована ли ФС
    uint8_t buf[FAT_SECTOR_SIZE];
    PY25Q16_ReadBuffer(0, buf, FAT_SECTOR_SIZE);
    
    fat_boot_sector_t *bs = (fat_boot_sector_t *)buf;
    if (bs->signature == 0xAA55 && bs->bytes_per_sector == FAT_SECTOR_SIZE) {
        fs_formatted = true;
    } else {
        // Форматируем при первом запуске
        FAT_Format();
        
        // Перечитываем для проверки
        PY25Q16_ReadBuffer(0, buf, FAT_SECTOR_SIZE);
        bs = (fat_boot_sector_t *)buf;
        fs_formatted = (bs->signature == 0xAA55);
    }
}

void FAT_Format(void) {
    uint8_t buf[FAT_SECTOR_SIZE];
    
    // 1. Создать и записать Boot Sector
    fat_boot_sector_t bs;
    FAT_CreateBootSector(&bs);
    
    // Стираем первый сектор
    PY25Q16_SectorErase(0);
    PY25Q16_WriteBuffer(0, &bs, sizeof(fat_boot_sector_t), false);
    
    // 2. Создать и записать FAT таблицу
    FAT_CreateFATTable(buf);
    
    // Стираем и пишем FAT
    uint32_t fat_addr = FAT_TABLE_SECTOR * FAT_SECTOR_SIZE;
    if ((fat_addr % 4096) == 0) {
        PY25Q16_SectorErase(fat_addr);
    }
    PY25Q16_WriteBuffer(fat_addr, buf, FAT_SECTOR_SIZE, false);
    
    // Остальные сектора FAT заполняем нулями
    memset(buf, 0, FAT_SECTOR_SIZE);
    for (int i = 1; i < FAT_SECTORS_PER_FAT; i++) {
        uint32_t addr = (FAT_TABLE_SECTOR + i) * FAT_SECTOR_SIZE;
        if ((addr % 4096) == 0) {
            PY25Q16_SectorErase(addr);
        }
        PY25Q16_WriteBuffer(addr, buf, FAT_SECTOR_SIZE, false);
    }
    
    // 3. Создать и записать Root Directory
    FAT_CreateRootDir(buf);
    
    uint32_t root_addr = FAT_ROOT_DIR_SECTOR * FAT_SECTOR_SIZE;
    if ((root_addr % 4096) == 0) {
        PY25Q16_SectorErase(root_addr);
    }
    PY25Q16_WriteBuffer(root_addr, buf, FAT_SECTOR_SIZE, false);
    
    // Остальные сектора root directory заполняем нулями
    memset(buf, 0, FAT_SECTOR_SIZE);
    for (int i = 1; i < FAT_ROOT_DIR_SECTORS; i++) {
        uint32_t addr = (FAT_ROOT_DIR_SECTOR + i) * FAT_SECTOR_SIZE;
        if ((addr % 4096) == 0) {
            PY25Q16_SectorErase(addr);
        }
        PY25Q16_WriteBuffer(addr, buf, FAT_SECTOR_SIZE, false);
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
    if ((addr % 4096) == 0) {
        PY25Q16_SectorErase(addr);
    }
    
    PY25Q16_WriteBuffer(addr, buf, FAT_SECTOR_SIZE, false);
}

