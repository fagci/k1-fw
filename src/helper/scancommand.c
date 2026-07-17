#include "scancommand.h"
#include "../driver/lfs.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../misc.h"

// ============================================================================
// Внутренние функции
// ============================================================================

static bool LoadCommand(SCMD_Context *ctx, SCMD_Command *cmd) {
  if (lfs_file_size(&gLfs, &ctx->file) <= ctx->file_pos) {
    return false;
  }

  int bytes = lfs_file_read(&gLfs, &ctx->file, cmd, sizeof(SCMD_Command));
  return (bytes == sizeof(SCMD_Command));
}

static bool HandleGoto(SCMD_Context *ctx) {
  uint16_t offset = ctx->current.goto_offset;

  Log("[SCMD] HandleGoto: offset=%u, current_index=%u", offset, ctx->cmd_index);

  // Защита от бесконечного цикла
  static uint16_t goto_count = 0;
  goto_count++;
  if (goto_count > 100) {
    Log("[SCMD] ERROR: Too many GOTOs, possible infinite loop");
    goto_count = 0;
    return false;
  }

  // Рассчитываем новую позицию
  lfs_soff_t target_pos = sizeof(SCMD_Header) + (offset * sizeof(SCMD_Command));

  // Проверяем границы
  lfs_soff_t file_size = lfs_file_size(&gLfs, &ctx->file);
  if (target_pos >= file_size) {
    Log("[SCMD] ERROR: GOTO target %lu beyond file size %ld", target_pos,
        file_size);
    goto_count = 0;
    return false;
  }

  // Перемещаемся
  lfs_file_seek(&gLfs, &ctx->file, target_pos, LFS_SEEK_SET);
  ctx->file_pos = target_pos;
  ctx->cmd_index = offset;

  // Загружаем команды
  if (!LoadCommand(ctx, &ctx->current)) {
    Log("[SCMD] ERROR: Cannot load command at target");
    goto_count = 0;
    return false;
  }

  ctx->has_next = LoadCommand(ctx, &ctx->next);

  Log("[SCMD] GOTO complete: cmd_index=%u, type=%d", ctx->cmd_index,
      ctx->current.type);

  goto_count = 0;
  return true;
}

// ============================================================================
// API функций
// ============================================================================

bool SCMD_Init(SCMD_Context *ctx, const char *filename) {
  if (!ctx)
    return false;

  memset(ctx, 0, sizeof(SCMD_Context));

  // Открываем файл с буфером
  struct lfs_file_config config = {.buffer = ctx->file_buffer, .attr_count = 0};

  int err =
      lfs_file_opencfg(&gLfs, &ctx->file, filename, LFS_O_RDONLY, &config);
  if (err < 0) {
    Log("[SCMD] Failed to open %s: %d", filename, err);
    return false;
  }

  // Читаем заголовок
  SCMD_Header header;
  int bytes = lfs_file_read(&gLfs, &ctx->file, &header, sizeof(header));
  if (bytes != sizeof(header)) {
    Log("[SCMD] Failed to read header");
    lfs_file_close(&gLfs, &ctx->file);
    return false;
  }

  if (header.magic != SCMD_MAGIC) {
    Log("[SCMD] Invalid magic: 0x%08X", header.magic);
    lfs_file_close(&gLfs, &ctx->file);
    return false;
  }

  ctx->file_pos = sizeof(SCMD_Header);
  ctx->cmd_index = 0;
  ctx->cmd_count = header.cmd_count;

  // Загружаем первую и вторую команды
  ctx->has_next = LoadCommand(ctx, &ctx->current);
  if (ctx->has_next) {
    LoadCommand(ctx, &ctx->next);
  }

  Log("[SCMD] Initialized: %u commands", ctx->cmd_count);
  return true;
}

void SCMD_Close(SCMD_Context *ctx) {
  if (!ctx)
    return;

  lfs_file_close(&gLfs, &ctx->file);
  memset(ctx, 0, sizeof(SCMD_Context));

  Log("[SCMD] Closed");
}

bool SCMD_Advance(SCMD_Context *ctx) {
  if (!ctx)
    return false;

  // Текущая становится предыдущей, следующая - текущей
  ctx->current = ctx->next;
  ctx->cmd_index++;
  ctx->file_pos += sizeof(SCMD_Command);

  Log("[SCMD] Advance: cmd_index=%u, goto_offset=%u", ctx->cmd_index,
      ctx->current.goto_offset);

  // Проверяем переходы
  if (ctx->current.goto_offset > 0) {
    Log("[SCMD] Executing GOTO to offset %u", ctx->current.goto_offset);
    return HandleGoto(ctx);
  }

  // Загружаем следующую команду
  ctx->has_next = LoadCommand(ctx, &ctx->next);

  if (!ctx->has_next) {
    Log("[SCMD] No more commands");
  }

  return ctx->has_next;
}

void SCMD_Rewind(SCMD_Context *ctx) {
  if (!ctx)
    return;

  lfs_file_seek(&gLfs, &ctx->file, sizeof(SCMD_Header), LFS_SEEK_SET);
  ctx->file_pos = sizeof(SCMD_Header);
  ctx->cmd_index = 0;
  ctx->call_ptr = 0;

  do {
    LoadCommand(ctx, &ctx->current);
    ctx->has_next = LoadCommand(ctx, &ctx->next);
  } while (ctx->current.skip);

  Log("[SCMD] Rewound to start");
}

SCMD_Command *SCMD_GetCurrent(SCMD_Context *ctx) {
  return ctx ? &ctx->current : NULL;
}

SCMD_Command *SCMD_GetNext(SCMD_Context *ctx) {
  return (ctx && ctx->has_next) ? &ctx->next : NULL;
}

bool SCMD_HasNext(SCMD_Context *ctx) { return ctx ? ctx->has_next : false; }

uint16_t SCMD_GetCurrentIndex(SCMD_Context *ctx) {
  return ctx ? ctx->cmd_index : 0;
}

uint16_t SCMD_GetCommandCount(SCMD_Context *ctx) {
  return ctx ? ctx->cmd_count : 0;
}

// ============================================================================
// Утилиты создания файлов
// ============================================================================

bool SCMD_CreateFile(const char *filename, SCMD_Command *commands,
                     uint16_t count) {
  uint8_t buffer[256];
  struct lfs_file_config config = {.buffer = buffer, .attr_count = 0};

  lfs_file_t file;
  int err = lfs_file_opencfg(&gLfs, &file, filename,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &config);
  if (err < 0) {
    Log("[SCMD] Failed to create %s: %d", filename, err);
    return false;
  }

  // Заголовок
  SCMD_Header header = {.magic = SCMD_MAGIC,
                        .version = SCMD_VERSION,
                        .cmd_count = count,
                        .entry_point = 0,
                        .crc32 = 0xDEADBEEF};

  lfs_ssize_t written = lfs_file_write(&gLfs, &file, &header, sizeof(header));
  if (written != sizeof(header)) {
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Команды
  written =
      lfs_file_write(&gLfs, &file, commands, sizeof(SCMD_Command) * count);
  bool success = (written == (lfs_ssize_t)(sizeof(SCMD_Command) * count));

  lfs_file_close(&gLfs, &file);

  Log("[SCMD] Created %s: %u commands", filename, count);
  return success;
}

void SCMD_CreateExampleScan(void) {
  struct lfs_info info;
  if (lfs_stat(&gLfs, "/scans", &info) < 0) {
    lfs_mkdir(&gLfs, "/scans");
  }

  SCMD_Command cmds[] = {
      // Метка начала
      {SCMD_MARKER, 0, 0, 0, 0, 0, 0, 0, 0, 0},

      // Диапазоны с автодобавлением в whitelist
      {SCMD_RANGE, 5, 0, 0, 20, 2000, 0, 0, 14400000, 17600000},
      {SCMD_RANGE, 5, 0, 0, 20, 2000, 0, 0, 40000000, 47000000},

      // Возврат к началу
      {SCMD_JUMP, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  };

  SCMD_CreateFile("/scans/cmd1.cmd", cmds, sizeof(cmds) / sizeof(cmds[0]));
  Log("[SCMD] Example scan file created");
}

void SCMD_DebugDumpFile(const char *filename) {
  uint8_t buffer[256];
  struct lfs_file_config config = {.buffer = buffer, .attr_count = 0};
  lfs_file_t file;

  int err = lfs_file_opencfg(&gLfs, &file, filename, LFS_O_RDONLY, &config);
  if (err < 0) {
    Log("[SCMD] Cannot open %s for debug", filename);
    return;
  }

  SCMD_Header header;
  lfs_file_read(&gLfs, &file, &header, sizeof(header));

  Log("[SCMD] === File %s ===", filename);
  Log("[SCMD] Magic: 0x%08X, Version: %u, Commands: %u", header.magic,
      header.version, header.cmd_count);

  for (uint16_t i = 0; i < header.cmd_count; i++) {
    SCMD_Command cmd;
    lfs_file_read(&gLfs, &file, &cmd, sizeof(cmd));

    Log("[SCMD] [%d] type=%d, prio=%d, flags=0x%02X, start=%lu, end=%lu, "
        "dwell=%u, goto=%u",
        i, cmd.type, cmd.priority, cmd.flags, cmd.start, cmd.end, cmd.dwell_ms,
        cmd.goto_offset);
  }

  lfs_file_close(&gLfs, &file);
  Log("[SCMD] === End of dump ===");
}
