#include "analysermenu.h"
#include "../driver/bk4829.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../misc.h"
#include "../ui/graphics.h"

// ---------------------------------------------------------------------------

static bool inMenu;

// ---------------------------------------------------------------------------
// Register descriptor
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  uint8_t reg;
  uint8_t shift;
  uint8_t width; // битовая ширина поля; 0 = весь регистр
  uint16_t maxVal; // 0 = без ограничения (для width=0 трактуется как 0xFFFF)
  uint16_t step; // шаг изменения / делитель для отображения
} RegDesc;

// Единственная точка конфигурации — добавь строку, и меню само её покажет.
// Формат: {name, reg, shift, width, maxVal, step}
static const RegDesc regDescs[] = {
    {"VCO LDO", 0x1F, 12, 4, 15, 1},        // REG_1F<15:12> vco_ldo_lvl
    {"vReg", 0x1A, 12, 4, 15, 1},           // REG_1A<15:12> vReg
    {"iBit", 0x1A, 8, 4, 15, 1},            // REG_1A<11:8> iBit
    {"pllCp", 0x1F, 0, 4, 15, 1},           //
    {"BndSelTh", 0x3E, 0, 16, 0xFFFF, 100}, //
                                            //
    {"DAC DRV2", 0x37, 8, 1, 1, 1},         // REG_37<14:12> dsp voltage
    {"RF LDO", 0x37, 9, 1, 1, 1},           // REG_37<14:12> dsp voltage
    {"ANA LDO", 0x37, 11, 1, 1, 1},         // REG_37<14:12> dsp voltage
    {"DSP V", 0x37, 12, 3, 7, 1},           // REG_37<14:12> dsp voltage
    {"GltchBW", 0x47, 1, 7, 0b1111111, 1},  //
    {"Compand", 0x31, 3, 1, 1, 1}, // enabling raises TX gain, drop MIC to compensate
    {"AVC", 0x4B, 5, 1, 1, 1}, // Automatic Volume Control (Rx)

    // --- PLL/VCO: сама перестройка (BK4819_TuneTo дёргает VCO_CALIB импульсом,
    // весь этот блок REG_30 определяет, что вообще включено на приёме) ---
    {"VCO Calib", 0x30, 15, 1, 1, 1},   // REG_30<15> impulse ENABLE_VCO_CALIB
    {"PLL VCO En", 0x30, 4, 4, 15, 1},  // REG_30<7:4> ENABLE_PLL_VCO (наш Reg30_SetPllVco)
    {"RX Link En", 0x30, 10, 4, 15, 1}, // REG_30<13:10> ENABLE_RX_LINK

    // --- IF-фильтр: полоса определяет постоянную времени, с которой
    // устаканивается Noise/Glitch (уже, — медленнее и селективнее; шире —
    // быстрее, но хуже отличает сигнал от шума) ---
    {"RF Filt BW", 0x43, 12, 3, 7, 1}, // REG_43<14:12> RF filter (rf[] в SetFilterBandwidth)
    {"WB Filt", 0x43, 9, 3, 7, 1},     // REG_43<11:9> weak-signal RF filter (wb[])
    {"BW Mode", 0x43, 4, 2, 3, 1},     // REG_43<5:4> 12.5k/6.25k/25k (bs[])

    // --- Входной РЧ-тракт: усиление каскадов (то, что при AGC=auto чип
    // крутит сам; REG_13 — верхняя строка таблицы усилений, см. GAIN_TABLE) ---
    {"LNA Short", 0x13, 8, 2, 3, 1}, // REG_13<9:8> LNA short-circuit gain
    {"LNA", 0x13, 5, 3, 7, 1},       // REG_13<7:5> LNA gain
    {"MIX", 0x13, 3, 2, 3, 1},       // REG_13<4:3> mixer gain
    {"PGA", 0x13, 0, 3, 7, 1},       // REG_13<2:0> PGA gain

    // --- IF-тракт: выбор ПЧ (BK4819_XtalSet пишет сюда ifset целиком,
    // BK4819_SetIfMode — 0x1C/0x1D) и ПОЛОСА DC-фильтра приёма — она
    // определяет, как быстро тракт восстанавливается после перестройки ---
    {"IF Sel", 0x3D, 0, 16, 0xFFFF, 100}, // REG_3D IF frequency select
    {"RX DCF", 0x7E, 3, 3, 7, 1},         // REG_7E<5:3> RX DC filter bandwidth

    // --- AFC: петля подстройки частоты ПОСЛЕ захвата PLL — влияет на то,
    // куда "уплывает" тракт на хвостах соседних сигналов ---
    {"AFC Dis", 0x73, 4, 1, 1, 1},    // REG_73<4> AFC disable
    {"AFC Range", 0x73, 11, 3, 7, 1}, // REG_73<13:11> AFC range select

    // --- AGC: пороги и скорость переключения усиления — источник
    // переходных процессов, на которых RSSI ещё не устаканился ---
    {"AGC Disable", 0x7E, 15, 1, 1, 1}, // REG_7E<15> 1=фиксированный gain, AGC выкл
    {"AGC Cnt", 0x7E, 12, 3, 7, 1},     // REG_7E<14:12> (в коде захардкожено на 3)
    {"AGC Hi", 0x49, 7, 7, 127, 1},     // REG_49<13:7> порог снижения усиления
    {"AGC Lo", 0x49, 0, 7, 127, 1},     // REG_49<6:0> порог повышения усиления
};

#define REG_COUNT ARRAY_SIZE(regDescs)

static MenuItem menuItems[REG_COUNT];

static Menu analyserMenu = {
    .title = "Settings",
    .items = menuItems,
    .itemHeight = 7,
    .width = 60,
    .x = LCD_WIDTH - 60,
};

// ---------------------------------------------------------------------------
// dBm range — publicly readable/writable, UI живёт в analyser.c
// ---------------------------------------------------------------------------

static int16_t dbmMin = -120;
static int16_t dbmMax = -20;

int16_t ANALYSERMENU_GetDbmMin(void) { return dbmMin; }
int16_t ANALYSERMENU_GetDbmMax(void) { return dbmMax; }
void ANALYSERMENU_SetDbmMin(int16_t v) { dbmMin = v; }
void ANALYSERMENU_SetDbmMax(int16_t v) { dbmMax = v; }

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

static uint16_t regRead(uint8_t idx) {
  const RegDesc *d = &regDescs[idx];
  uint16_t regVal = BK4819_ReadRegister(d->reg);
  if (d->width == 0)
    return regVal;
  return (regVal >> d->shift) & ((1u << d->width) - 1u);
}

static void regWrite(uint8_t idx, uint16_t val) {
  const RegDesc *d = &regDescs[idx];
  if (d->width == 0) {
    BK4819_WriteRegister(d->reg, val);
  } else {
    uint16_t regVal = BK4819_ReadRegister(d->reg);
    uint16_t mask = ((1u << d->width) - 1u) << d->shift;
    uint16_t newVal = (regVal & ~mask) | ((val << d->shift) & mask);
    BK4819_WriteRegister(d->reg, newVal);
  }
  gRedrawScreen = true;
}

static uint16_t regMaxVal(uint8_t idx) {
  const RegDesc *d = &regDescs[idx];
  if (d->maxVal)
    return d->maxVal;
  if (d->width)
    return (1u << d->width) - 1u;
  return 0xFFFF;
}

// ---------------------------------------------------------------------------
// Menu callbacks
// ---------------------------------------------------------------------------

static void getVal(const MenuItem *item, char *buf, uint8_t buf_size) {
  uint8_t idx = (uint8_t)(uintptr_t)item->submenu;
  uint16_t v = regRead(idx);
  uint16_t s = regDescs[idx].step;
  snprintf(buf, buf_size, "%5u", (s > 1) ? (unsigned)(v / s) : (unsigned)v);
}

static void updateVal(const MenuItem *item, bool up) {
  uint8_t idx = (uint8_t)(uintptr_t)item->submenu;
  uint16_t v = regRead(idx);
  uint16_t maxV = regMaxVal(idx);
  uint16_t step = regDescs[idx].step;
  if (step == 0)
    step = 1;

  if (up) {
    v = (v + step > maxV) ? 0 : v + step;
  } else {
    v = (v < step) ? maxV : v - step;
  }
  regWrite(idx, v);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

static void renderAnalyserMenuItem(uint16_t index, uint8_t visIndex) {
  const MenuItem *item = &analyserMenu.items[index];
  const uint8_t y = analyserMenu.y + visIndex * analyserMenu.itemHeight;
  const uint8_t by = y + analyserMenu.itemHeight - 2;

  char value_buf[8];
  item->get_value_text(item, value_buf, sizeof(value_buf));

  // Имя слева от рамки, значение справа — не слипается
  PrintSmall(analyserMenu.x + 2, by, "%s", item->name);
  PrintSmallEx(analyserMenu.x + analyserMenu.width - 2, by, POS_R, C_FILL, "%s",
               value_buf);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

static void initMenu(void) {
  for (uint8_t i = 0; i < REG_COUNT; ++i) {
    MenuItem *m = &menuItems[i];
    m->name = regDescs[i].name;
    m->setting = 0;
    m->get_value_text = getVal;
    m->change_value = updateVal;
    m->submenu = (struct Menu *)(uintptr_t)i;
    m->action = NULL;
  }

  analyserMenu.num_items = REG_COUNT;
  analyserMenu.render_item = renderAnalyserMenuItem;
  // Раньше .y бралось из SPECTRUM_Y — общей переменной, которую каждое
  // приложение (Analyser, VFO1 со своим графиком) выставляет под себя.
  // Это меню открывается из обоих, и позиция "уползала" в зависимости от
  // того, что SPECTRUM_Y значило в момент открытия. Фиксируем позицию.
  analyserMenu.y = MENU_Y;
  // height было = REG_COUNT * itemHeight, т.е. под ВСЕ пункты сразу,
  // независимо от того, помещаются ли они на экран — при .y от VFO1 и
  // /или росте REG_COUNT строки просто уезжали за нижний край. Отдаём
  // под меню весь остаток экрана — тогда работает штатный скролл Menu.
  analyserMenu.height = LCD_HEIGHT - analyserMenu.y;
  MENU_Init(&analyserMenu);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ANALYSERMENU_Draw(void) {
  if (inMenu) {
    MENU_Render();
  }
}

bool ANALYSERMENU_Key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED && key == KEY_EXIT && inMenu) {
    inMenu = false;
    MENU_Deinit();
    return true;
  }

  if (inMenu && MENU_HandleInput(key, state)) {
    return true;
  }

  return inMenu;
}

// Called from analyser.c on long KEY_0
bool ANALYSERMENU_Toggle(void) {
  inMenu = !inMenu;
  if (inMenu) {
    initMenu();
  } else {
    MENU_Deinit();
  }
  return inMenu;
}

bool ANALYSERMENU_IsActive(void) { return inMenu; }
