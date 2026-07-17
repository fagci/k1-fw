#include "lootlist.h"
#include "../dcs.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../external/printf/printf.h"
#include "../inc/band.h"
#include "../radio.h"
#include "bands.h"
#include "storage.h"
#include <stdint.h>

static Loot loot[LOOT_SIZE_MAX] = {0};
static uint32_t lastTimeCheck = 0;
static int16_t lootIndex = -1;

Loot *gLastActiveLoot = NULL;
int16_t gLastActiveLootIndex = -1;
static uint32_t lastActiveLootF =
    0; // частота для восстановления указателя после сортировки

void LOOT_BlacklistLast(void) {
  if (gLastActiveLoot) {
    gLastActiveLoot->whitelist = false;
    gLastActiveLoot->blacklist = true;
  }
}

void LOOT_WhitelistLast(void) {
  if (gLastActiveLoot) {
    gLastActiveLoot->blacklist = false;
    gLastActiveLoot->whitelist = true;
  }
}

Loot *LOOT_Get(uint32_t f) {
  for (uint16_t i = 0; i < LOOT_Size(); ++i) {
    if ((&loot[i])->f == f) {
      return &loot[i];
    }
  }
  return NULL;
}

int16_t LOOT_IndexOf(Loot *item) {
  for (uint16_t i = 0; i < LOOT_Size(); ++i) {
    if (&loot[i] == item) {
      return i;
    }
  }
  return -1;
}

Loot *LOOT_AddEx(uint32_t f, bool reuse) {
  if (reuse) {
    Loot *p = LOOT_Get(f);
    if (p) {
      return p;
    }
  }
  if (LOOT_Size() >= LOOT_SIZE_MAX) {
    // FIFO-вытеснение: убираем самый старый незащищённый слот.
    // whitelist/blacklist помечены пользователем — не трогаем.
    int16_t evict = -1;
    for (uint16_t i = 0; i < LOOT_Size(); ++i) {
      if (!loot[i].whitelist && !loot[i].blacklist) {
        evict = (int16_t)i;
        break;
      }
    }
    if (evict < 0) {
      return NULL; // все слоты защищены — отказ от добавления
    }
    LOOT_Remove((uint16_t)evict);
  }
  lootIndex++;
  lastTimeCheck = Now();
  loot[lootIndex] = (Loot){
      .f = f,
      .lastTimeOpen = (uint16_t)(Now() / 1000),
      .duration = 0,
      .code = 0xFF,
      .open = true, // as we add it when open
  };
  return &loot[lootIndex];
}

Loot *LOOT_Add(uint32_t f) { return LOOT_AddEx(f, true); }

void LOOT_Remove(uint16_t i) {
  if (!LOOT_Size())
    return;
  if (gLastActiveLoot == &loot[i]) {
    gLastActiveLoot = NULL;
    gLastActiveLootIndex = -1;
    lastActiveLootF = 0;
  }
  for (; i < LOOT_Size() - 1; ++i)
    loot[i] = loot[i + 1];
  lootIndex--;
  // Сдвиг массива инвалидировал указатель — перепривязываем по частоте
  if (lastActiveLootF) {
    gLastActiveLoot = LOOT_Get(lastActiveLootF);
    gLastActiveLootIndex = gLastActiveLoot ? LOOT_IndexOf(gLastActiveLoot) : -1;
  }
}

void LOOT_Clear(void) {
  lootIndex = -1;
  gLastActiveLoot = NULL;
  gLastActiveLootIndex = -1;
  lastActiveLootF = 0;
}

uint16_t LOOT_Size(void) { return lootIndex + 1; }

uint32_t LOOT_SecondsAgo(const Loot *loot) {
  // lastTimeOpen хранится в секундах (Now()/1000), а Now() — в мс, поэтому
  // сравнивать их напрямую нельзя: разница окажется задавлена величиной
  // Now() и будет почти одинаковой для всех записей. Сначала переводим
  // Now() в секунды, uint16_t-арифметика корректно оборачивается через ~18ч.
  const uint16_t now_s = (uint16_t)(Now() / 1000);
  return (uint16_t)(now_s - loot->lastTimeOpen);
}

void LOOT_Standby(void) {
  for (uint16_t i = 0; i < LOOT_Size(); ++i) {
    Loot *p = &loot[i];
    p->open = false;
  }
  lastTimeCheck = Now();
}

static void swap(Loot *a, Loot *b) {
  Loot tmp = *a;
  *a = *b;
  *b = tmp;
}

bool LOOT_SortByLastOpenTime(const Loot *a, const Loot *b) {
  return a->lastTimeOpen < b->lastTimeOpen;
}

bool LOOT_SortByDuration(const Loot *a, const Loot *b) {
  return a->duration > b->duration;
}

bool LOOT_SortByF(const Loot *a, const Loot *b) { return a->f > b->f; }

bool LOOT_SortByBlacklist(const Loot *a, const Loot *b) {
  return a->blacklist > b->blacklist;
}

static void Sort(Loot *items, uint16_t n,
                 bool (*compare)(const Loot *a, const Loot *b), bool reverse) {
  for (uint16_t i = 0; i < n - 1; i++) {
    bool swapped = false;
    for (uint16_t j = 0; j < n - i - 1; j++) {
      if (compare(&items[j], &items[j + 1]) ^ reverse) {
        swap(&items[j], &items[j + 1]);
        swapped = true;
      }
    }
    if (!swapped) {
      break;
    }
  }
}

void LOOT_Sort(bool (*compare)(const Loot *a, const Loot *b), bool reverse) {
  Sort(loot, LOOT_Size(), compare, reverse);
  // После сортировки указатель мог сместиться — восстанавливаем по частоте
  if (lastActiveLootF) {
    gLastActiveLoot = LOOT_Get(lastActiveLootF);
    gLastActiveLootIndex = gLastActiveLoot ? LOOT_IndexOf(gLastActiveLoot) : -1;
  }
}

Loot *LOOT_Item(uint16_t i) { return &loot[i]; }

void LOOT_Replace(Measurement *item, uint32_t f) {
  item->f = f;
  item->open = false;
  item->lastTimeOpen = 0;
  item->duration = 0;
  item->snr = 0;
  item->rssi = 0;
  item->noise = UINT8_MAX;
  item->glitch = UINT8_MAX;
  item->code = 0xFF;
  lastTimeCheck = Now();
}

void LOOT_UpdateEx(Loot *item, Measurement *msm) {
  if (item == NULL) {
    return;
  }

  if (item->blacklist || (item->whitelist && !gMonitorMode)) {
    msm->open = false;
  }

  // item->snr = msm->snr;

  if (item->open) {
    // Аккумулируем мс пока активен один и тот же loot, переносим целые
    // секунды в item->duration. Сброс при смене активного — теряем <1 сек.
    static const Loot *durationOwner = NULL;
    static uint16_t durationFracMs = 0;

    if (durationOwner != item) {
      durationOwner = item;
      durationFracMs = 0;
    }
    uint32_t dms = Now() - lastTimeCheck;
    durationFracMs += (uint16_t)(dms > 999 ? 999 : dms); // защита от заскока
    while (durationFracMs >= 1000) {
      durationFracMs -= 1000;
      if (item->duration < 0xFFFF)
        item->duration++;
    }

    gLastActiveLoot = item;
    gLastActiveLootIndex = LOOT_IndexOf(item);
    lastActiveLootF = item->f;
  }
  if (msm->open) {
    item->lastTimeOpen = (uint16_t)(Now() / 1000);
    uint32_t cd = 0;
    uint16_t ct = 0;
    uint8_t Code = 0;
    BK4819_CssScanResult_t res = BK4819_GetCxCSSScanResult(&cd, &ct);
    msm->isCd = false;
    switch (res) {
    case BK4819_CSS_RESULT_CDCSS:
      msm->code = DCS_GetCdcssCode(cd);
      msm->isCd = true;
      break;
    case BK4819_CSS_RESULT_CTCSS:
      msm->code = DCS_GetCtcssCode(ct);
      break;
    default:
      msm->code = 255;
      break;
    }
  }
  lastTimeCheck = Now();
  item->open = msm->open;
  item->code = msm->code;
  item->isCd = msm->isCd;

  if (msm->blacklist) {
    item->blacklist = true;
  }

  item->modulation = RADIO_GetParam(ctx, PARAM_MODULATION);
  item->bw = RADIO_GetParam(ctx, PARAM_BANDWIDTH);
  item->gainIndex = RADIO_GetParam(ctx, PARAM_GAIN);
  item->radio = RADIO_GetParam(ctx, PARAM_RADIO);
  item->squelch_type = RADIO_GetParam(ctx, PARAM_SQUELCH_TYPE);
  item->squelch_value = RADIO_GetParam(ctx, PARAM_SQUELCH_VALUE);
}

void LOOT_Update(Measurement *msm) {
  Loot *item = LOOT_Get(msm->f);

  if (item == NULL && msm->open) {
    item = LOOT_Add(msm->f);
  }

  LOOT_UpdateEx(item, msm);
}

void LOOT_RemoveBlacklisted(void) {
  LOOT_Sort(LOOT_SortByBlacklist, true);
  for (uint16_t i = 0; i < LOOT_Size(); ++i) {
    if (loot[i].blacklist) {
      lootIndex = i;
      return;
    }
  }
}

CH LOOT_ToCh(const Loot *loot) {
  // TODO: automatic params by simple "band plan"
  Band p = BANDS_ByFrequency(loot->f);
  CH ch = {
      .rxF = loot->f,
      .txF = 0,
      .code =
          (CodeRXTX){
              .rx.type = CODE_TYPE_OFF,
              .tx.type = CODE_TYPE_OFF,
              .rx.value = 0,
              .tx.value = 0,
          },
      .radio = p.radio,
      .modulation = p.modulation,
      .power = p.power,
      .bw = p.bw,
      .squelch = p.squelch,
      .gainIndex = p.gainIndex,
  };

  mhzToS(ch.name, ch.rxF);

  if (loot->code != 255) {
    ch.code.tx.value = loot->code;
    if (loot->isCd) {
      ch.code.tx.type = CODE_TYPE_DIGITAL;
    } else {
      ch.code.tx.type = CODE_TYPE_CONTINUOUS_TONE;
    }
  }

  ch.bw = loot->bw;
  ch.radio = loot->radio;
  ch.gainIndex = loot->gainIndex;
  ch.squelch.type = loot->squelch_type;
  ch.squelch.value = loot->squelch_value;
  ch.modulation = loot->modulation;

  return ch;
}

// ============================================================================
// Persistence: save/load loot list to/from file
// ============================================================================

// Magic привязан к sizeof(Loot). При смене раскладки структуры — bump.
#define LOOT_FILE_MAGIC 0x4C54u // 'LT'
#define LOOT_FILE_VERSION 3

typedef struct {
  uint16_t magic;
  uint16_t version;
  uint16_t count;
  uint16_t loot_size; // sizeof(Loot) — доп. защита
} LootFileHeader;

bool LOOT_SaveToFile(const char *filename) {
  LootFileHeader hdr = {
      .magic = LOOT_FILE_MAGIC,
      .version = LOOT_FILE_VERSION,
      .count = LOOT_Size(),
      .loot_size = sizeof(Loot),
  };
  if (!Storage_Save(filename, 0, &hdr, sizeof(hdr))) {
    return false;
  }
  if (hdr.count == 0) {
    return true;
  }
  return Storage_SaveMultiple(filename, 1, loot, sizeof(Loot), hdr.count);
}

bool LOOT_LoadFromFile(const char *filename) {
  LootFileHeader hdr = {0};

  if (!Storage_Load(filename, 0, &hdr, sizeof(hdr))) {
    return false;
  }

  // Несовместимый формат — чистим и игнорируем
  if (hdr.magic != LOOT_FILE_MAGIC || hdr.version != LOOT_FILE_VERSION ||
      hdr.loot_size != sizeof(Loot) || hdr.count > LOOT_SIZE_MAX) {
    LOOT_Clear();
    return true;
  }

  if (hdr.count == 0) {
    LOOT_Clear();
    return true;
  }

  if (!Storage_LoadMultiple(filename, 1, loot, sizeof(Loot), hdr.count)) {
    return false;
  }

  lootIndex = (int16_t)hdr.count - 1;

  // open — runtime-флаг, после загрузки всегда сброшен
  for (uint16_t i = 0; i < hdr.count; ++i) {
    loot[i].open = 0;
  }

  // gLastActiveLoot нельзя восстановить из файла — указатель невалиден
  gLastActiveLoot = NULL;
  gLastActiveLootIndex = -1;
  lastActiveLootF = 0;

  return true;
}

// Default filename
#define LOOT_DEFAULT_FILE "Loot.loot"

bool LOOT_Save(void) { return LOOT_SaveToFile(LOOT_DEFAULT_FILE); }

bool LOOT_Load(void) { return LOOT_LoadFromFile(LOOT_DEFAULT_FILE); }
