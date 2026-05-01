#include "cmdedit.h"
#include "../driver/lfs.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../helper/scan.h"
#include "../helper/scancommand.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <string.h>

#define SCMD_MAX_COMMANDS 16

// Small text: 5px высота, baseline = нижний пиксель
// Medium text: 7px высота
// EDIT_LINE_H: шаг строк в режиме редактирования
#define EDIT_LINE_H 7
#define EDIT_START_Y 17 // baseline первого поля (после заголовка+черты)

typedef enum {
  MODE_LIST,
  MODE_EDIT,
} EditorMode;

typedef struct {
  uint16_t totalCommands;
  SCMD_Command commands[SCMD_MAX_COMMANDS];
  char filename[32];
  bool modified;
  EditorMode mode;
  uint8_t editField;
} EditContext;

static EditContext gEditCtx;
static Menu cmdMenu;
static uint8_t selected_index;

// Устанавливается из cmdscan.c перед APPS_run(APP_CMDEDIT)
char gCmdEditFilename[32] = "/scans/cmd1.cmd";

// ============================================================================
// Поля редактирования
// ============================================================================

static uint8_t getMaxField(uint8_t type) {
  switch (type) {
  case SCMD_RANGE:
    return 5; // type,range(start+end),step,dwell,prio,flags
  case SCMD_CHANNEL:
    return 4; // type,start,dwell,prio,flags
  case SCMD_JUMP:
  case SCMD_CJUMP:
    return 1; // type,goto
  case SCMD_PAUSE:
    return 1; // type,dwell
  default:
    return 0;
  }
}

// ============================================================================
// Файловые операции
// ============================================================================

static void LoadFile(const char *filename) {
  strcpy(gEditCtx.filename, filename);

  uint8_t buffer[256];
  struct lfs_file_config config = {.buffer = buffer, .attr_count = 0};
  lfs_file_t file;

  int err = lfs_file_opencfg(&gLfs, &file, filename, LFS_O_RDONLY, &config);
  if (err < 0) {
    Log("[CMDEDIT] Open failed: %d (%s)", err, filename);
    gEditCtx.totalCommands = 0;
    return;
  }

  SCMD_Header header;
  lfs_file_read(&gLfs, &file, &header, sizeof(header));

  if (header.magic != SCMD_MAGIC) {
    Log("[CMDEDIT] Bad magic: 0x%08X", header.magic);
    lfs_file_close(&gLfs, &file);
    gEditCtx.totalCommands = 0;
    return;
  }

  gEditCtx.totalCommands = header.cmd_count;
  if (gEditCtx.totalCommands > SCMD_MAX_COMMANDS)
    gEditCtx.totalCommands = SCMD_MAX_COMMANDS;

  for (uint16_t i = 0; i < gEditCtx.totalCommands; i++)
    lfs_file_read(&gLfs, &file, &gEditCtx.commands[i], sizeof(SCMD_Command));

  lfs_file_close(&gLfs, &file);
  gEditCtx.modified = false;
  Log("[CMDEDIT] Loaded %u cmds from %s", gEditCtx.totalCommands, filename);
}

static void ShowMsg(const char *msg) {
  FillRect(0, LCD_YCENTER - 5, LCD_WIDTH, 10, C_FILL);
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER + 3, POS_C, C_INVERT, msg);
  ST7565_Blit();
  SYSTICK_DelayMs(800);
}

static void SaveFile(void) {
  if (gEditCtx.filename[0] == 0) {
    Log("[CMDEDIT] SaveFile: no filename");
    return;
  }

  // Создаём /scans/ если нет
  struct lfs_info info;
  if (lfs_stat(&gLfs, "/scans", &info) < 0) {
    int r = lfs_mkdir(&gLfs, "/scans");
    Log("[CMDEDIT] mkdir /scans: %d", r);
  }

  // SCAN держит файл открытым через SCMD_Context — закрываем перед записью.
  // Если lfs_open всё равно возвращает ошибку, нужно добавить в scan.c
  // функцию SCAN_CloseCommandFile() которая вызывает SCMD_Close().
  bool wasActive = SCAN_IsCommandMode();
  if (wasActive) {
    Log("[CMDEDIT] Stopping SCAN before save");
    SCAN_SetCommandMode(false);
  }

  uint8_t filebuf[256];
  struct lfs_file_config cfg = {.buffer = filebuf, .attr_count = 0};
  lfs_file_t file;

  int err = lfs_file_opencfg(&gLfs, &file, gEditCtx.filename,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &cfg);
  Log("[CMDEDIT] lfs_open(W): %d, file=%s", err, gEditCtx.filename);
  if (err < 0) {
    if (wasActive)
      SCAN_LoadCommandFile(gEditCtx.filename);
    ShowMsg("Save ERR!");
    return;
  }

  SCMD_Header hdr = {
      .magic = SCMD_MAGIC,
      .version = SCMD_VERSION,
      .cmd_count = gEditCtx.totalCommands,
      .entry_point = 0,
      .crc32 = 0xDEADBEEF,
  };

  lfs_ssize_t w1 = lfs_file_write(&gLfs, &file, &hdr, sizeof(hdr));
  lfs_ssize_t w2 =
      lfs_file_write(&gLfs, &file, gEditCtx.commands,
                     sizeof(SCMD_Command) * gEditCtx.totalCommands);
  lfs_file_close(&gLfs, &file);

  Log("[CMDEDIT] w_hdr=%d w_cmds=%d (need %d+%d)", (int)w1, (int)w2,
      (int)sizeof(hdr), (int)(sizeof(SCMD_Command) * gEditCtx.totalCommands));

  bool ok =
      (w1 == (lfs_ssize_t)sizeof(hdr)) &&
      (w2 == (lfs_ssize_t)(sizeof(SCMD_Command) * gEditCtx.totalCommands));

  if (ok) {
    gEditCtx.modified = false;
    Log("[CMDEDIT] Saved OK: %u cmds to %s", gEditCtx.totalCommands,
        gEditCtx.filename);
  }

  if (wasActive)
    SCAN_LoadCommandFile(gEditCtx.filename);

  ShowMsg(ok ? "Saved!" : "Save ERR!");
}

// ============================================================================
// Операции с командами
// ============================================================================

static void AddCommand(void) {
  if (gEditCtx.totalCommands >= SCMD_MAX_COMMANDS)
    return;
  SCMD_Command newCmd = {
      .type = SCMD_CHANNEL,
      .start = 100000000,
      .end = 100000000,
      .step = 12500,
      .dwell_ms = 100,
      .priority = 0,
      .flags = 0,
  };
  gEditCtx.commands[gEditCtx.totalCommands++] = newCmd;
  gEditCtx.modified = true;
  cmdMenu.num_items = gEditCtx.totalCommands;
}
static void CommentCommand(uint16_t index) {
  if (index >= gEditCtx.totalCommands)
    return;

  gEditCtx.commands[index].skip = !gEditCtx.commands[index].skip;
  gEditCtx.modified = true;
}

static void DeleteCommand(uint16_t index) {
  if (index >= gEditCtx.totalCommands)
    return;
  for (uint16_t i = index; i < gEditCtx.totalCommands - 1; i++)
    gEditCtx.commands[i] = gEditCtx.commands[i + 1];
  gEditCtx.totalCommands--;
  gEditCtx.modified = true;
  cmdMenu.num_items = gEditCtx.totalCommands;
}

static void DuplicateCommand(uint16_t index) {
  if (gEditCtx.totalCommands >= SCMD_MAX_COMMANDS ||
      index >= gEditCtx.totalCommands)
    return;
  gEditCtx.commands[gEditCtx.totalCommands++] = gEditCtx.commands[index];
  gEditCtx.modified = true;
  cmdMenu.num_items = gEditCtx.totalCommands;
}

// ============================================================================
// Callbacks для FINPUT
// ============================================================================

static void cbSetFreq(uint32_t fs, uint32_t fe) {
  gEditCtx.commands[selected_index].start = fs;
  gEditCtx.commands[selected_index].end = fe;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void cbSetDwell(uint32_t dwell, uint32_t _) {
  gEditCtx.commands[selected_index].dwell_ms = dwell;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void cbSetStep(uint32_t step, uint32_t _) {
  gEditCtx.commands[selected_index].step = step;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void cbSetGoto(uint32_t offset, uint32_t _) {
  gEditCtx.commands[selected_index].goto_offset = (uint16_t)offset;
  gEditCtx.modified = true;
  gFInputActive = false;
}

// ============================================================================
// Редактирование поля
// ============================================================================

static void EditCommandField(uint16_t index, uint8_t field) {
  SCMD_Command *cmd = &gEditCtx.commands[index];

  if (field == 0) {
    cmd->type = (cmd->type + 1) % SCMD_COUNT;
    gEditCtx.editField = 0;
    gEditCtx.modified = true;
    return;
  }

  switch (cmd->type) {
  case SCMD_CHANNEL:
    switch (field) {
    case 1:
      gFInputCallback = cbSetFreq;
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
      gFInputValue1 = cmd->start;
      gFInputValue2 = 0;
      FINPUT_init();
      gFInputActive = true;
      break;
    case 2:
      gFInputCallback = cbSetDwell;
      FINPUT_setup(0, 60000, UNIT_MS, false);
      gFInputValue1 = cmd->dwell_ms;
      FINPUT_init();
      gFInputActive = true;
      break;
    case 3:
      cmd->priority = (cmd->priority + 1) % 10;
      gEditCtx.modified = true;
      break;
    case 4:
      cmd->flags ^= SCMD_FLAG_AUTO_WHITELIST;
      gEditCtx.modified = true;
      break;
    }
    break;

  case SCMD_RANGE:
    switch (field) {
    case 1: // range: start+end вместе, как в scaner.c setRange
      gFInputCallback = cbSetFreq;
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
      gFInputValue1 = cmd->start;
      gFInputValue2 = cmd->end;
      FINPUT_init();
      gFInputActive = true;
      break;
    case 2: // step
      gFInputCallback = cbSetStep;
      FINPUT_setup(1, 100000, UNIT_KHZ, false);
      gFInputValue1 = cmd->step;
      FINPUT_init();
      gFInputActive = true;
      break;
    case 3: // dwell
      gFInputCallback = cbSetDwell;
      FINPUT_setup(0, 60000, UNIT_MS, false);
      gFInputValue1 = cmd->dwell_ms;
      FINPUT_init();
      gFInputActive = true;
      break;
    case 4: // priority
      cmd->priority = (cmd->priority + 1) % 10;
      gEditCtx.modified = true;
      break;
    case 5: // flags
      cmd->flags ^= SCMD_FLAG_AUTO_WHITELIST;
      gEditCtx.modified = true;
      break;
    }
    break;

  case SCMD_JUMP:
  case SCMD_CJUMP:
    if (field == 1) {
      gFInputCallback = cbSetGoto;
      FINPUT_setup(0, 65535, UNIT_HZ, false);
      gFInputValue1 = cmd->goto_offset;
      FINPUT_init();
      gFInputActive = true;
    }
    break;

  case SCMD_PAUSE:
    if (field == 1) {
      gFInputCallback = cbSetDwell;
      FINPUT_setup(0, 60000, UNIT_MS, false);
      gFInputValue1 = cmd->dwell_ms;
      FINPUT_init();
      gFInputActive = true;
    }
    break;
  }
}

// ============================================================================
// Отображение списка
// ============================================================================

static void renderCommandItem(uint16_t index, uint8_t i) {
  const SCMD_Command *cmd = &gEditCtx.commands[index];
  const uint8_t ty = MENU_Y + i * MENU_ITEM_H + 7;

  PrintMediumEx(2, ty, POS_L, C_FILL, "%u:%s", index + 1,
                SCMD_NAMES_SHORT[cmd->type]);

  if (cmd->skip) {
    DrawHLine(0, ty - 3, LCD_WIDTH - 4, C_FILL);
  }

  switch (cmd->type) {
  case SCMD_RANGE:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "%lu-%lu", cmd->start / KHZ,
                 cmd->end / KHZ);
    PrintSmallEx(LCD_WIDTH - 2, ty, POS_R, C_INVERT, "%luk", cmd->step / KHZ);
    break;
  case SCMD_CHANNEL:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "%lu.%05lu", cmd->start / MHZ,
                 cmd->start % MHZ);
    break;
  case SCMD_PAUSE:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "%ums", cmd->dwell_ms);
    break;
  case SCMD_JUMP:
  case SCMD_CJUMP:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "->%u", cmd->goto_offset);
    break;
  case SCMD_MARKER:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "[%u]", index);
    break;
  default:
    break;
  }

  if (cmd->priority > 0)
    PrintSmallEx(LCD_WIDTH - 2, ty, POS_R, C_INVERT, "P%u", cmd->priority);
  else if (cmd->flags & SCMD_FLAG_AUTO_WHITELIST)
    PrintSmallEx(LCD_WIDTH - 2, ty, POS_R, C_INVERT, "W");
}

// ============================================================================
// Отображение режима редактирования
// ============================================================================

// Baseline y для поля f
#define FIELD_Y(f) (EDIT_START_Y + (f) * EDIT_LINE_H)

// Строка поля: сначала FillRect подсветит, потом рисуем текст
#define ROW(f, fmt, ...)                                                       \
  PrintSmallEx(3, FIELD_Y(f), POS_L, sel == (f) ? C_INVERT : C_FILL, fmt,      \
               ##__VA_ARGS__)

static void renderEditMode(void) {
  if (gFInputActive) {
    FINPUT_render();
    return;
  }

  uint16_t index = selected_index;
  if (index >= gEditCtx.totalCommands)
    return;

  SCMD_Command *cmd = &gEditCtx.commands[index];
  uint8_t sel = gEditCtx.editField;

  // Заголовок: medium 7px, baseline y=8, пиксели строк 2..8
  PrintMediumEx(LCD_XCENTER, 8, POS_C, C_FILL, "#%u %s%s", index + 1,
                SCMD_NAMES_SHORT[cmd->type], gEditCtx.modified ? "*" : "");

  // Черта под заголовком: y=10
  DrawLine(0, 10, LCD_WIDTH - 1, 10, C_FILL);

  // Подсветка выбранной строки:
  // baseline = FIELD_Y(sel), small text: пиксели [baseline-4..baseline]
  // rect: top = baseline-5, height = 7 (по 1px отступа сверху и снизу)
  FillRect(0, FIELD_Y(sel) - 5, LCD_WIDTH, 7, C_FILL);

  // Поле 0: тип (всегда)
  ROW(0, "Type: %s", SCMD_NAMES[cmd->type]);

  // Поля 1..N по типу команды
  // При lineH=7, startY=17: позиции 17,24,31,38,45,52,59 — все внутри 64px
  switch (cmd->type) {
  case SCMD_CHANNEL:
    ROW(1, "Freq: %lu.%05lu", cmd->start / MHZ, cmd->start % MHZ);
    ROW(2, "Dwell: %ums", cmd->dwell_ms);
    ROW(3, "Prio: %u", cmd->priority);
    ROW(4, "AutoWL: %s",
        (cmd->flags & SCMD_FLAG_AUTO_WHITELIST) ? "ON" : "OFF");
    break;

  case SCMD_RANGE:
    // Диапазон в одну строку, вызов как в scaner.c (FINPUT с isRange=true)
    ROW(1, "%lu.%03lu-%lu.%03lu", cmd->start / MHZ, (cmd->start % MHZ) / 1000,
        cmd->end / MHZ, (cmd->end % MHZ) / 1000);
    ROW(2, "Step: %luHz", cmd->step * 10);
    ROW(3, "Dwell: %ums", cmd->dwell_ms);
    ROW(4, "Prio: %u", cmd->priority);
    ROW(5, "AutoWL: %s",
        (cmd->flags & SCMD_FLAG_AUTO_WHITELIST) ? "ON" : "OFF");
    break;

  case SCMD_JUMP:
  case SCMD_CJUMP:
    ROW(1, "Goto: %u", cmd->goto_offset);
    break;

  case SCMD_PAUSE:
    ROW(1, "Dwell: %ums", cmd->dwell_ms);
    break;

  default:
    break;
  }
}

#undef ROW
#undef FIELD_Y

// ============================================================================
// Обработка ввода
// ============================================================================

static bool listModeAction(const uint16_t index, KEY_Code_t key,
                           Key_State_t state) {
  if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_0:
      gEditCtx.totalCommands = 0;
      gEditCtx.modified = true;
      cmdMenu.num_items = 0;
      return true;
    case KEY_F:
      SaveFile();
      return true;
    default:
      break;
    }
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      if (gEditCtx.modified)
        SaveFile();
      APPS_exit();
      return true;
    case KEY_MENU:
      gEditCtx.mode = MODE_EDIT;
      gEditCtx.editField = 0;
      selected_index = index;
      return true;
    case KEY_F:
      SaveFile();
      return true;
    case KEY_STAR:
      SCAN_LoadCommandFile(gEditCtx.filename);
      return true;
    case KEY_1:
      AddCommand();
      return true;
    case KEY_2:
      DuplicateCommand(index);
      return true;
    case KEY_3:
      CommentCommand(index);
      return true;
    case KEY_0:
      DeleteCommand(index);
      return true;
    default:
      break;
    }
  }

  return false;
}

static bool editModeKey(KEY_Code_t key, Key_State_t state) {
  if (gFInputActive)
    return false;

  uint16_t index = selected_index;
  if (index >= gEditCtx.totalCommands)
    return false;

  SCMD_Command *cmd = &gEditCtx.commands[index];

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      gEditCtx.mode = MODE_LIST;
      return true;
    case KEY_MENU:
      EditCommandField(index, gEditCtx.editField);
      return true;
    case KEY_UP:
      if (gEditCtx.editField > 0)
        gEditCtx.editField--;
      return true;
    case KEY_DOWN:
      if (gEditCtx.editField < getMaxField(cmd->type))
        gEditCtx.editField++;
      return true;
    case KEY_F:
      SaveFile();
      return true;
    default:
      break;
    }
  }

  return false;
}

bool CMDEDIT_key(KEY_Code_t key, Key_State_t state) {
  if (gEditCtx.mode == MODE_EDIT)
    return editModeKey(key, state);

  if (MENU_HandleInput(key, state))
    return true;

  return false;
}

// ============================================================================
// Отображение (список)
// ============================================================================

void CMDEDIT_render(void) {
  if (gEditCtx.mode == MODE_EDIT) {
    renderEditMode();
    return;
  }

  if (gEditCtx.totalCommands == 0) {
    PrintMediumEx(LCD_XCENTER, 30, POS_C, C_FILL, "No commands");
    PrintSmallEx(LCD_XCENTER, 42, POS_C, C_FILL, "1:Add  F:Save");
    return;
  }

  MENU_Render();

  // Подсказка: baseline y=63, пиксели 59..63 — самый низ экрана
  PrintSmallEx(2, 63, POS_L, C_FILL, "MENU:Edit 1:+ 2:Dup 0:Del F:Save");
}

// ============================================================================
// Инициализация
// ============================================================================

void CMDEDIT_SetFilename(const char *filename) {
  strncpy(gCmdEditFilename, filename, sizeof(gCmdEditFilename) - 1);
  gCmdEditFilename[sizeof(gCmdEditFilename) - 1] = 0;
}

static void initMenu(void) {
  cmdMenu.num_items = gEditCtx.totalCommands;
  cmdMenu.itemHeight = MENU_ITEM_H;
  cmdMenu.title = "Commands";
  cmdMenu.render_item = renderCommandItem;
  cmdMenu.action = listModeAction;
  MENU_Init(&cmdMenu);
}

void CMDEDIT_init(void) {
  memset(&gEditCtx, 0, sizeof(gEditCtx));
  gEditCtx.mode = MODE_LIST;
  LoadFile(gCmdEditFilename);
  initMenu();
  STATUSLINE_SetText("CMD: %s", gCmdEditFilename);
}

void CMDEDIT_update(void) {
  if (gEditCtx.modified)
    STATUSLINE_SetText("CMD: %s*", gEditCtx.filename);
}
