#ifndef LOOTLIST_H
#define LOOTLIST_H

#include "../driver/keyboard.h"
#include "../helper/measurements.h"
#include "../inc/channel.h"
#include "../inc/loot.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef LOOT_SIZE_MAX
#define LOOT_SIZE_MAX 250
#endif

extern Loot *gLastActiveLoot;
extern int16_t gLastActiveLootIndex;
extern bool gLootlistActive;

void LOOT_BlacklistLast(void);
void LOOT_WhitelistLast(void);
Loot *LOOT_Get(uint32_t f);
int16_t LOOT_IndexOf(Loot *item);
Loot *LOOT_Add(uint32_t f);
Loot *LOOT_AddEx(uint32_t f, bool reuse);
void LOOT_Remove(uint16_t i);
void LOOT_Clear(void);
uint16_t LOOT_Size(void);
uint32_t LOOT_SecondsAgo(const Loot *loot);
void LOOT_Standby(void);

bool LOOT_SortByLastOpenTime(const Loot *a, const Loot *b);
bool LOOT_SortByDuration(const Loot *a, const Loot *b);
bool LOOT_SortByF(const Loot *a, const Loot *b);
bool LOOT_SortByBlacklist(const Loot *a, const Loot *b);
void LOOT_Sort(bool (*compare)(const Loot *a, const Loot *b), bool reverse);

Loot *LOOT_Item(uint16_t i);
void LOOT_Replace(Measurement *item, uint32_t f);
void LOOT_UpdateEx(Loot *item, Measurement *msm);
void LOOT_Update(Measurement *msm);
void LOOT_RemoveBlacklisted(void);

CH LOOT_ToCh(const Loot *loot);

bool LOOT_Save(void);
bool LOOT_Load(void);
bool LOOT_SaveToFile(const char *filename);
bool LOOT_LoadFromFile(const char *filename);

void LOOTLIST_init(void);
void LOOTLIST_update(void);
void LOOTLIST_render(void);
bool LOOTLIST_key(KEY_Code_t key, Key_State_t state);
void LOOTLIST_StartEdit(uint16_t index);

#endif
