#include "files.h"
#include "../driver/lfs.h"
#include "../driver/uart.h"
#include "../helper/menu.h"
#include "../helper/screenshot.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include "files.h"
#include <string.h>

// Increased MAX_NAME_LEN from 12 to 16 to fit filenames like "Settings.set" (12
// chars + null) Memory: 12 × 24 = 288 bytes (reduced from 480)
#define MAX_FILES 12
#define MAX_PATH_LEN 64
#define MAX_NAME_LEN                                                           \
  16 // 15 символов + '\0'; достаточно для имён типа "Settings.set"

typedef enum {
  FILE_TYPE_FILE = 0,
  FILE_TYPE_FOLDER,
  FILE_TYPE_VFO,
  FILE_TYPE_BAND,
  FILE_TYPE_CH,
  FILE_TYPE_SET,
  FILE_TYPE_SL,
  FILE_TYPE_BACK,
} FileType;

typedef struct {
  char name[MAX_NAME_LEN];
  uint32_t size; // для папок = 0
  uint8_t type; // FileType; uint8_t вместо int экономит выравнивание
} FileEntry; // sizeof = 16+4+1+pad(3) = 24 байт

static FileEntry gFilesList[MAX_FILES];
static uint16_t gFilesCount = 0;
static char gCurrentPath[MAX_PATH_LEN];
static char gStatusText[32];

static bool showingScreenshot;
static char screenshotPath[32];

static const Symbol fileTypeIcons[] = {
    [FILE_TYPE_FILE] = SYM_FILE,   [FILE_TYPE_FOLDER] = SYM_FOLDER,
    [FILE_TYPE_BACK] = SYM_MISC2,  [FILE_TYPE_VFO] = SYM_VFO,
    [FILE_TYPE_BAND] = SYM_BAND,   [FILE_TYPE_CH] = SYM_CH,
    [FILE_TYPE_SET] = SYM_SETTING, [FILE_TYPE_SL] = SYM_SCAN,
};

static void loadDirectory(const char *path);
static void renderItem(uint16_t index, uint8_t i);
static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state);
static void navigateTo(const char *name);
static void formatSize(uint32_t size, char *buffer, uint8_t bufferSize);

static Menu filesMenu = {
    .render_item = renderItem,
    .itemHeight = MENU_ITEM_H,
    .action = action,
    .num_items = 0,
};

static void formatSize(uint32_t size, char *buffer, uint8_t bufferSize) {
  if (size < 1024) {
    snprintf(buffer, bufferSize, "%u B", size);
  } else if (size < 1024 * 1024) {
    snprintf(buffer, bufferSize, "%u KB", size / 1024);
  } else {
    snprintf(buffer, bufferSize, "%u MB", size / (1024 * 1024));
  }
}

static void deleteItem(const char *name, FileType type) {
  (void)type; // lfs_remove работает одинаково для файлов и папок
  char fullPath[MAX_PATH_LEN];

  // Construct path without leading slash for root-level files
  // to match how storage.c creates files (e.g., "Settings.set" not
  // "/Settings.set")
  if (strcmp(gCurrentPath, "/") == 0) {
    snprintf(fullPath, sizeof(fullPath), "%s", name);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%s/%s", gCurrentPath, name);
  }

  int err = lfs_remove(&gLfs, fullPath);
  if (err < 0) {
    char msg[32];
    snprintf(msg, sizeof(msg), "Delete error: %d", err);
    STATUSLINE_SetText(msg);
  } else {
    STATUSLINE_SetText("Deleted");
    loadDirectory(gCurrentPath);
  }
}

static const char *getFileExtension(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename)
    return "";
  return dot + 1;
}

static void loadDirectory(const char *path) {
  lfs_dir_t dir;
  struct lfs_info info;
  gFilesCount = 0;

  int err = lfs_dir_open(&gLfs, &dir, path);
  if (err < 0) {
    if (strcmp(path, "/") == 0) {
      strcpy(gFilesList[0].name, "..");
      gFilesList[0].type = FILE_TYPE_BACK;
      gFilesList[0].size = 0;
      gFilesCount = 1;
    }
    strncpy(gCurrentPath, path, sizeof(gCurrentPath));
    return;
  }

  if (strcmp(path, "/") != 0) {
    strcpy(gFilesList[gFilesCount].name, "..");
    gFilesList[gFilesCount].type = FILE_TYPE_BACK;
    gFilesList[gFilesCount].size = 0;
    gFilesCount++;
  }

  while (lfs_dir_read(&gLfs, &dir, &info) == 1) {
    if (gFilesCount >= MAX_FILES)
      break;
    if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0)
      continue;

    strncpy(gFilesList[gFilesCount].name, info.name, MAX_NAME_LEN - 1);
    gFilesList[gFilesCount].name[MAX_NAME_LEN - 1] = '\0';

    if (info.type == LFS_TYPE_DIR) {
      gFilesList[gFilesCount].type = FILE_TYPE_FOLDER;
      gFilesList[gFilesCount].size = 0;
    } else {
      const char *ext = getFileExtension(gFilesList[gFilesCount].name);
      if (strcmp(ext, "vfo") == 0)
        gFilesList[gFilesCount].type = FILE_TYPE_VFO;
      else if (strcmp(ext, "bnd") == 0)
        gFilesList[gFilesCount].type = FILE_TYPE_BAND;
      else if (strcmp(ext, "ch") == 0)
        gFilesList[gFilesCount].type = FILE_TYPE_CH;
      else if (strcmp(ext, "set") == 0)
        gFilesList[gFilesCount].type = FILE_TYPE_SET;
      else if (strcmp(ext, "sl") == 0)
        gFilesList[gFilesCount].type = FILE_TYPE_SL;
      else
        gFilesList[gFilesCount].type = FILE_TYPE_FILE;
      gFilesList[gFilesCount].size = info.size;
    }
    gFilesCount++;
  }

  lfs_dir_close(&gLfs, &dir);

  // Сортировка: папки вперёд, затем алфавит
  for (uint16_t i = 0; i < gFilesCount - 1; i++) {
    for (uint16_t j = 0; j < gFilesCount - i - 1; j++) {
      bool swap = false;
      if (gFilesList[j].type != FILE_TYPE_FOLDER &&
          gFilesList[j + 1].type == FILE_TYPE_FOLDER) {
        swap = true;
      } else if (gFilesList[j].type == gFilesList[j + 1].type) {
        if (strcmp(gFilesList[j].name, gFilesList[j + 1].name) > 0)
          swap = true;
      }
      if (swap) {
        FileEntry temp = gFilesList[j];
        gFilesList[j] = gFilesList[j + 1];
        gFilesList[j + 1] = temp;
      }
    }
  }

  filesMenu.num_items = gFilesCount;
  MENU_Init(&filesMenu);

  strncpy(gCurrentPath, path, sizeof(gCurrentPath) - 1);
  gCurrentPath[sizeof(gCurrentPath) - 1] = '\0';

  snprintf(gStatusText, sizeof(gStatusText), "%u items", gFilesCount);
}

static void navigateTo(const char *name) {
  char newPath[MAX_PATH_LEN];

  if (strcmp(name, "..") == 0) {
    char *lastSlash = strrchr(gCurrentPath, '/');
    if (lastSlash != NULL) {
      if (lastSlash == gCurrentPath) {
        strcpy(newPath, "/");
      } else {
        *lastSlash = '\0';
        strcpy(newPath, gCurrentPath);
        if (strlen(newPath) == 0)
          strcpy(newPath, "/");
      }
    }
  } else {
    if (strcmp(gCurrentPath, "/") == 0)
      snprintf(newPath, sizeof(newPath), "/%s", name);
    else
      snprintf(newPath, sizeof(newPath), "%s/%s", gCurrentPath, name);
  }

  struct lfs_info info;
  int err = lfs_stat(&gLfs, newPath, &info);

  if (err == 0 && info.type == LFS_TYPE_DIR) {
    loadDirectory(newPath);
    STATUSLINE_SetText("%s", gStatusText);
  } else {
    char sizeStr[16];
    formatSize(info.size, sizeStr, sizeof(sizeStr));
    STATUSLINE_SetText("%s - %s", name, sizeStr);
    const char *ext = getFileExtension(name);
    if (strcmp(ext, "sq") == 0) {
      // Open squelch editor with this file
      char fullPath[MAX_PATH_LEN];
      if (strcmp(gCurrentPath, "/") == 0) {
        snprintf(fullPath, sizeof(fullPath), "/%s", name);
      } else {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", gCurrentPath, name);
      }
      APPS_runWithFile(APP_SQVIEWER, fullPath);
      return;
    }
    if (strcmp(ext, "ch") == 0) {
      // Open channel list editor with this file
      char fullPath[MAX_PATH_LEN];
      if (strcmp(gCurrentPath, "/") == 0) {
        snprintf(fullPath, sizeof(fullPath), "/%s", name);
      } else {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", gCurrentPath, name);
      }
      APPS_runWithFile(APP_CHLIST, fullPath);
      return;
    }
    if (strcmp(ext, "cmd") == 0) {
      // Open command editor with this file
      char fullPath[MAX_PATH_LEN];
      if (strcmp(gCurrentPath, "/") == 0) {
        snprintf(fullPath, sizeof(fullPath), "/%s", name);
      } else {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", gCurrentPath, name);
      }
      APPS_runWithFile(APP_CMDEDIT, fullPath);
      return;
    }
    if (strcmp(ext, "bmp") == 0) {
      showingScreenshot = true;
      snprintf(screenshotPath, sizeof(screenshotPath), "%s/%s", gCurrentPath,
               name);
    }
  }
}

static void renderItem(uint16_t index, uint8_t i) {
  if (index >= gFilesCount)
    return;

  FileEntry *entry = &gFilesList[index];
  uint8_t y = MENU_Y + i * MENU_ITEM_H;
  uint8_t x_offset = 2;

  if (fileTypeIcons[entry->type] != 0) {
    PrintSymbolsEx(x_offset, y + 8, POS_L, C_INVERT, "%c",
                   fileTypeIcons[entry->type]);
    x_offset += 13;
  }

  PrintMediumEx(x_offset, y + 8, POS_L, C_INVERT, "%s", entry->name);

  if (entry->type == FILE_TYPE_FILE) {
    char sizeStr[16];
    formatSize(entry->size, sizeStr, sizeof(sizeStr));
    PrintSmallEx(LCD_WIDTH - 5, y + 7, POS_R, C_INVERT, "%s", sizeStr);
  }
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (index >= gFilesCount)
    return false;

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_PTT:
    case KEY_MENU:
      navigateTo(gFilesList[index].name);
      return true;

    case KEY_5:
      loadDirectory(gCurrentPath);
      STATUSLINE_SetText("%s", gStatusText);
      return true;

    case KEY_1:
      STATUSLINE_SetText("Create folder - NYI");
      return true;

    case KEY_0:
      deleteItem(gFilesList[index].name, (FileType)gFilesList[index].type);
      return true;

    case KEY_EXIT:
      if (strcmp(gCurrentPath, "/") == 0) {
        APPS_exit();
        return true;
      }
      navigateTo("..");
      return true;

    default:
      break;
    }
  }

  return false;
}

void FILES_init() {
  gCurrentPath[0] = '/';
  gCurrentPath[1] = '\0';

  loadDirectory(gCurrentPath);

  if (gFilesCount == 0)
    STATUSLINE_SetText("Empty or no filesystem");
  else
    STATUSLINE_SetText("%s", gStatusText);
}

void FILES_deinit() {
  gFilesCount = 0;
  memset(gFilesList, 0, sizeof(gFilesList));
}

bool FILES_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED) {
    if (showingScreenshot) {
      if (key == KEY_EXIT) {
        showingScreenshot = false;
        return true;
      }
    }

    switch (key) {
    case KEY_STAR:
      APPS_exit();
      return true;

    default:
      break;
    }
  }

  if (MENU_HandleInput(key, state))
    return true;

  return false;
}

void FILES_render() {
  if (showingScreenshot) {
    displayScreen(screenshotPath);
    FillRect(0, 0, LCD_WIDTH, 8, C_FILL);
    PrintSmall(1, 5, "Screenshot");
    return;
  }
  MENU_Render();
  PrintMediumEx(2, 2, POS_L, C_FILL, "%s", gCurrentPath);
}
