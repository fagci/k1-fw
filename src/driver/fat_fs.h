#ifndef FAT_FS_H
#define FAT_FS_H

#include <stdint.h>
#include <stdbool.h>

// FAT16 конфигурация для 2MB флеш
#define FAT_SECTOR_SIZE 512
#define FAT_SECTORS_PER_CLUSTER 8  // 4KB кластер
#define FAT_RESERVED_SECTORS 1
#define FAT_NUM_FATS 1  // Одна таблица FAT для экономии места
#define FAT_ROOT_ENTRIES 512
#define FAT_TOTAL_SECTORS 4096  // 2MB / 512 байт

// Вычисляемые параметры
#define FAT_ROOT_DIR_SECTORS ((FAT_ROOT_ENTRIES * 32 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE)
#define FAT_SECTORS_PER_FAT 16  // ~2048 кластеров для FAT16

// Смещения секторов
#define FAT_BOOT_SECTOR 0
#define FAT_TABLE_SECTOR (FAT_BOOT_SECTOR + FAT_RESERVED_SECTORS)
#define FAT_ROOT_DIR_SECTOR (FAT_TABLE_SECTOR + (FAT_NUM_FATS * FAT_SECTORS_PER_FAT))
#define FAT_DATA_SECTOR (FAT_ROOT_DIR_SECTOR + FAT_ROOT_DIR_SECTORS)

// FAT Entry типы
#define FAT_FREE_CLUSTER 0x0000
#define FAT_RESERVED_CLUSTER 0xFFF0
#define FAT_BAD_CLUSTER 0xFFF7
#define FAT_EOC 0xFFFF  // End of chain

// Атрибуты файлов
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE 0x20

// Boot Sector структура
typedef struct __attribute__((packed)) {
    uint8_t jmp_boot[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
    uint8_t boot_code[448];
    uint16_t signature;
} fat_boot_sector_t;

// Directory Entry структура
typedef struct __attribute__((packed)) {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_res;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat_dir_entry_t;

// Публичные функции
void FAT_Init(void);
void FAT_Format(void);
void FAT_ReadBlock(uint32_t lba, uint8_t *buf);
void FAT_WriteBlock(uint32_t lba, const uint8_t *buf);

#endif // FAT_FS_H
