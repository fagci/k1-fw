// cmdscan.c
#include "cmdscan.h"

#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include "cmdedit.h"

// =============================
// Состояние приложения
// =============================
typedef struct {
  bool isActive;      // Приложение активно
  uint8_t profileNum; // Текущий профиль (1-4)
  uint16_t cmdIndex;  // Индекс текущей команды
  uint32_t execCount; // Счетчик выполненных команд
  char filename[32];  // Имя текущего файла
} CmdScanState;

static CmdScanState cmdState = {.isActive = false,
                                .profileNum = 1,
                                .cmdIndex = 0,
                                .execCount = 0,
                                .filename = "/scans/cmd1.cmd"};

// =============================
// Вспомогательные функции
// =============================

// Загрузить профиль по номеру
static void LoadProfile(uint8_t num) {
  snprintf(cmdState.filename, sizeof(cmdState.filename), "/scans/cmd%d.cmd",
           num);

  // Закрываем предыдущий файл если был открыт
  if (SCAN_IsCommandMode()) {
    SCAN_SetCommandMode(false);
  }

  // Загружаем новый файл через SCAN API
  SCAN_LoadCommandFile(cmdState.filename);

  if (SCAN_IsCommandMode()) {
    cmdState.profileNum = num;
    cmdState.cmdIndex = 0;
    cmdState.execCount = 0;
    Log("[CMDSCAN] Loaded profile %d: %s", num, cmdState.filename);
  } else {
    Log("[CMDSCAN] Failed to load %s", cmdState.filename);
    // Пробуем загрузить по умолчанию
    if (num != 1) {
      LoadProfile(1);
    }
  }
}

// Отобразить информацию о текущей команде
static void RenderCommandInfo(void) {
  // Используем API из scan.c для получения текущей команды
  SCMD_Command *cmd = SCAN_GetCurrentCommand();
  if (!cmd)
    return;
  cmdState.cmdIndex = SCAN_GetCommandIndex();

  // Координаты для отображения
  int y = 30;

  // Тип команды

  PrintSmallEx(2, y, POS_L, C_FILL, "CMD: %s P:%d",
               SCMD_NAMES_SHORT[cmd->type % 16], cmd->priority);
  y += 8;

  // Параметры в зависимости от типа
  switch (cmd->type) {
  case SCMD_CHANNEL:
    PrintSmallEx(2, y, POS_L, C_FILL, "F: %.3f MHz", cmd->start / 1000000.0f);
    y += 8;
    PrintSmallEx(2, y, POS_L, C_FILL, "Dwell: %dms", cmd->dwell_ms);
    break;

  case SCMD_RANGE:
    PrintSmallEx(2, y, POS_L, C_FILL, "R: %.3f-%.3f", cmd->start / 1000000.0f,
                 cmd->end / 1000000.0f);
    y += 8;
    PrintSmallEx(2, y, POS_L, C_FILL, "Step: %dms", cmd->dwell_ms);
    break;

  case SCMD_PAUSE:
    PrintSmallEx(2, y, POS_L, C_FILL, "Pause: %dms", cmd->dwell_ms);
    break;

  case SCMD_JUMP:
  case SCMD_CJUMP:
    PrintSmallEx(2, y, POS_L, C_FILL, "Jump to: %d", cmd->goto_offset);
    y += 8;
    // PrintSmallEx(2, y, POS_L, C_FILL, "Loops: %d", cmd->loop_count);
    break;
  }

  // Флаги
  if (cmd->flags) {
    y += 8;
    PrintSmallEx(2, y, POS_L, C_FILL, "Flags: 0x%02X", cmd->flags);
  }

  // Статистика
  y = LCD_HEIGHT - 20;
  PrintSmallEx(2, y, POS_L, C_FILL, "Profile: %d", cmdState.profileNum);

  PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_FILL, "#%d", cmdState.cmdIndex);

  y += 8;
  PrintSmallEx(2, y, POS_L, C_FILL, "Exec: %lu", cmdState.execCount);

  // Индикатор паузы
  if (SCAN_IsCommandMode()) {
    /* if (SCAN_IsCommandPaused()) {
      PrintMediumEx(LCD_XCENTER, LCD_HEIGHT - 30, POS_C, C_INVERT,
                        "PAUSED");
    } */
  }
}

// =============================
// API функций приложения
// =============================

void CMDSCAN_init(void) {
  // Переключаемся в режим VFO для командного сканирования
  SCAN_SetMode(SCAN_MODE_SINGLE);

  // Загружаем первый профиль через SCAN API
  LoadProfile(cmdState.profileNum);

  cmdState.isActive = true;
  cmdState.execCount = 0;

  Log("[CMDSCAN] Initialized");
}

void CMDSCAN_deinit(void) {
  // Возвращаем обычный режим через SCAN API
  SCAN_SetCommandMode(false);
  cmdState.isActive = false;
}

void CMDSCAN_update(void) {
  if (cmdState.isActive) {
    // Обновляем статистику
    SCMD_Command *cmd = SCAN_GetCurrentCommand();
    if (cmd) {
      cmdState.cmdIndex = SCAN_GetCommandIndex();
    }
  }
}

bool CMDSCAN_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state)) {
    return true;
  }
  if (state == KEY_RELEASED) {
    switch (key) {
    // Цифры 1-4: выбор профиля
    case KEY_1:
      LoadProfile(1);
      return true;
    case KEY_2:
      LoadProfile(2);
      return true;
    case KEY_3:
      LoadProfile(3);
      return true;
    case KEY_4:
      LoadProfile(4);
      return true;
    case KEY_9:
      // Принудительный переход к следующей команде
      SCAN_CommandForceNext();
      return true;

    case KEY_F:
      CMDEDIT_SetFilename(cmdState.filename); // добавить эту строку
      APPS_run(APP_CMDEDIT);
      return true;

    // Управление выполнением
    case KEY_UP:
      // Шаг вперед - переходим к следующей команде
      if (SCAN_IsCommandMode()) {
        SCAN_CommandForceNext();
        cmdState.execCount++;
      }
      return true;

      /* case KEY_DOWN:
        // Перемотка в начало через SCAN API
        SCAN_CommandRewind();
        cmdState.cmdIndex = 0;
        cmdState.execCount = 0;
        return true; */

    case KEY_SIDE1:
      LOOT_BlacklistLast();
      return true;

    case KEY_SIDE2:
      LOOT_WhitelistLast();
      return true;

    case KEY_STAR:
      LOOTLIST_init();
      gLootlistActive = true;
      return true;

    case KEY_EXIT:
      // Выход из приложения
      APPS_exit();
      return true;

    case KEY_PTT:
      APPS_run(APP_VFO1);
      return true;
    default:
      break;
    }
  }

  // Долгое нажатие
  if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_STAR:
      // Создать тестовый файл (если есть место)
      SCMD_CreateExampleScan();
      Log("[CMDSCAN] Created example file");
      // Перезагружаем текущий профиль
      LoadProfile(cmdState.profileNum);
      return true;

      /* case KEY_EXIT:
        // Полный сброс
        SCAN_CommandRewind();
        cmdState.execCount = 0;
        cmdState.cmdIndex = 0;
        return true; */
    default:
      break;
    }
  }

  return false;
}

void CMDSCAN_render(void) {
  STATUSLINE_RenderRadioSettings();

  // Отображаем имя файла (укороченное)
  const char *filename = strrchr(cmdState.filename, '/');
  if (filename) {
    filename++;
  } else {
    filename = cmdState.filename;
  }

  char freqBuf[16];
  SCMD_Command *cmd = SCAN_GetCurrentCommand();

  PrintSmallEx(LCD_XCENTER, 12, POS_C, C_FILL, "%s", filename);

  if (cmd) {
    const char *typeNames[] = {
        [SCMD_NOP] = "--",     // NOP
        [SCMD_CHANNEL] = "CH", // Одиночный канал
        [SCMD_RANGE] = "RNG",  // Диапазон частот
        [SCMD_JUMP] = "JMP",   // Безусловный переход
        [SCMD_CJUMP] = "CJ", // Условный переход (если сигнал)
        [SCMD_PAUSE] = "PAU",    // Пауза
        [SCMD_CALL] = "CAL",     // Вызов подпрограммы
        [SCMD_RETURN] = "RET",   // Возврат из подпрограммы
        [SCMD_MARKER] = "MRK",   // Метка для переходов
        [SCMD_SETPRIO] = "PRIO", // Установка приоритета
        [SCMD_SETMODE] = "MOD",  // Установка режима
    };
    PrintSmallEx(LCD_XCENTER, 12 + 8, POS_C, C_FILL, "Cmd: %d/%d %s .%lu",
                 cmdState.cmdIndex, SCAN_GetCommandCount(),
                 typeNames[cmd->type], cmdState.execCount);
  }

  mhzToS(freqBuf, vfo->msm.f);
  PrintMediumEx(LCD_XCENTER, 12 + 8 + 8, POS_C, C_FILL, "%s", freqBuf);

  if (vfo->is_open) {
    UI_RSSIBar(41);
  }

  if (gLastActiveLoot) {
    UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 40, POS_C);
  }
  REGSMENU_Draw();
}
