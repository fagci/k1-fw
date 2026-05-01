#include "graphics.h"
#include "../misc.h"
#include "fonts/NumbersStepanv3.h"
#include "fonts/TomThumb.h"
#include "fonts/muMatrix8ptRegular.h"
#include "fonts/symbols.h"
#include <stdlib.h>
#include <string.h>

static Cursor cursor;

static const GFXfont *const fonts[] = {&TomThumb, &MuMatrix8ptRegular, &dig_14};

void UI_ClearStatus(void) { FillRect(0, 0, LCD_WIDTH, 7, C_CLEAR); }
void UI_ClearScreen(void) {
  FillRect(0, 7, LCD_WIDTH, LCD_HEIGHT - 7, C_CLEAR);
}

// ---------------------------------------------------------------------------
// Вспомогательный макрос: пометить страницу грязной.
// gLineChanged[] объявлен в st7565.c / st7565.h как extern bool[FRAME_LINES].
// ---------------------------------------------------------------------------
#define MARK_DIRTY(page)                       \
  do {                                         \
    if ((page) < FRAME_LINES)                  \
      gLineChanged[(page)] = true;             \
  } while (0)

// ---------------------------------------------------------------------------
// PutPixel
// ---------------------------------------------------------------------------
inline void PutPixel(uint8_t x, uint8_t y, uint8_t fill) {
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
    return;
  uint8_t page = y >> 3;
  uint8_t m = 1 << (y & 7);
  uint8_t *p = &gFrameBuffer[page][x];
  if (fill) {
    if (fill & 2)
      *p ^= m;
    else
      *p |= m;
  } else {
    *p &= ~m;
  }
  MARK_DIRTY(page);
}

bool GetPixel(uint8_t x, uint8_t y) {
  return gFrameBuffer[y >> 3][x] & (1 << (y & 7));
}

// ---------------------------------------------------------------------------
// Bresenham (диагональные линии) — dirty по каждому пикселю через PutPixel
// ---------------------------------------------------------------------------
static void DrawALine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t c) {
  int16_t s = abs(y1 - y0) > abs(x1 - x0);
  if (s) {
    SWAP(x0, y0);
    SWAP(x1, y1);
  }
  if (x0 > x1) {
    SWAP(x0, x1);
    SWAP(y0, y1);
  }

  int16_t dx = x1 - x0, dy = abs(y1 - y0), e = dx >> 1,
          ys = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++, e -= dy) {
    PutPixel(s ? y0 : x0, s ? x0 : y0, c);
    if (e < 0) {
      y0 += ys;
      e += dx;
    }
  }
}

// ---------------------------------------------------------------------------
// DrawVLine — dirty по всем затронутым страницам
// ---------------------------------------------------------------------------
void DrawVLine(int16_t x, int16_t y, int16_t h, Color c) {
  if (x < 0 || x >= LCD_WIDTH || h <= 0)
    return;
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > LCD_HEIGHT)
    h = LCD_HEIGHT - y;
  if (h <= 0)
    return;

  uint8_t startPage = y >> 3;
  uint8_t endPage = (y + h - 1) >> 3;

  if (startPage == endPage) {
    uint8_t yBit = y & 7;
    uint8_t mask = ((1 << h) - 1) << yBit;

    if (c == C_CLEAR)
      gFrameBuffer[startPage][x] &= ~mask;
    else if (c == C_FILL)
      gFrameBuffer[startPage][x] |= mask;
    else
      gFrameBuffer[startPage][x] ^= mask;

    MARK_DIRTY(startPage);
  } else {
    uint8_t topMask = 0xFF << (y & 7);
    uint8_t bottomBits = (y + h) & 7;
    uint8_t bottomMask = bottomBits ? (1 << bottomBits) - 1 : 0xFF;

    if (c == C_CLEAR) {
      gFrameBuffer[startPage][x] &= ~topMask;
      for (uint8_t p = startPage + 1; p < endPage; p++) {
        gFrameBuffer[p][x] = 0;
        MARK_DIRTY(p);
      }
      if (bottomBits)
        gFrameBuffer[endPage][x] &= ~bottomMask;
      else
        gFrameBuffer[endPage][x] = 0;
    } else if (c == C_FILL) {
      gFrameBuffer[startPage][x] |= topMask;
      for (uint8_t p = startPage + 1; p < endPage; p++) {
        gFrameBuffer[p][x] = 0xFF;
        MARK_DIRTY(p);
      }
      if (bottomBits)
        gFrameBuffer[endPage][x] |= bottomMask;
      else
        gFrameBuffer[endPage][x] = 0xFF;
    } else {
      gFrameBuffer[startPage][x] ^= topMask;
      for (uint8_t p = startPage + 1; p < endPage; p++) {
        gFrameBuffer[p][x] ^= 0xFF;
        MARK_DIRTY(p);
      }
      if (bottomBits)
        gFrameBuffer[endPage][x] ^= bottomMask;
      else
        gFrameBuffer[endPage][x] ^= 0xFF;
    }

    MARK_DIRTY(startPage);
    MARK_DIRTY(endPage);
  }
}

// ---------------------------------------------------------------------------
// DrawHLine — одна страница, одна пометка
// ---------------------------------------------------------------------------
void DrawHLine(int16_t x, int16_t y, int16_t w, Color c) {
  if (y < 0 || y >= LCD_HEIGHT || w <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (x + w > LCD_WIDTH)
    w = LCD_WIDTH - x;
  if (w <= 0)
    return;

  uint8_t page = y >> 3;
  uint8_t mask = 1 << (y & 7);
  uint8_t *p = &gFrameBuffer[page][x];

  if (c == C_CLEAR) {
    mask = ~mask;
    for (int16_t i = 0; i < w; i++)
      p[i] &= mask;
  } else if (c == C_FILL) {
    for (int16_t i = 0; i < w; i++)
      p[i] |= mask;
  } else {
    for (int16_t i = 0; i < w; i++)
      p[i] ^= mask;
  }

  MARK_DIRTY(page);
}

// ---------------------------------------------------------------------------
// DrawLine
// ---------------------------------------------------------------------------
void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) {
  if (x0 == x1) {
    if (y0 > y1)
      SWAP(y0, y1);
    DrawVLine(x0, y0, y1 - y0 + 1, c);
  } else if (y0 == y1) {
    if (x0 > x1)
      SWAP(x0, x1);
    DrawHLine(x0, y0, x1 - x0 + 1, c);
  } else {
    DrawALine(x0, y0, x1, y1, c);
  }
}

void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
  DrawHLine(x, y, w, c);
  DrawHLine(x, y + h - 1, w, c);
  DrawVLine(x, y, h, c);
  DrawVLine(x + w - 1, y, h, c);
}

// ---------------------------------------------------------------------------
// FillRect — dirty по всем затронутым страницам одним проходом
// ---------------------------------------------------------------------------
void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  if (w <= 0 || h <= 0)
    return;

  uint8_t startPage = y >> 3;
  uint8_t endPage   = (y + h - 1) >> 3;

  if (c == C_CLEAR) {
    if (startPage == endPage) {
      uint8_t topBits    = y & 7;
      uint8_t bottomBits = (y + h - 1) & 7;
      uint8_t mask = ~((0xFF << topBits) & (0xFF >> (7 - bottomBits)));
      for (int16_t i = x; i < x + w; i++)
        gFrameBuffer[startPage][i] &= mask;
    } else {
      for (uint8_t page = startPage; page <= endPage; page++) {
        if (page == startPage && (y & 7)) {
          uint8_t mask = (1 << (y & 7)) - 1;
          for (int16_t i = x; i < x + w; i++)
            gFrameBuffer[page][i] &= mask;
        } else if (page == endPage && ((y + h) & 7)) {
          uint8_t mask = ~((1 << ((y + h) & 7)) - 1);
          for (int16_t i = x; i < x + w; i++)
            gFrameBuffer[page][i] &= mask;
        } else {
          memset(&gFrameBuffer[page][x], 0, w);
        }
        MARK_DIRTY(page);
      }
      return; // dirty уже выставлен внутри цикла
    }
  } else {
    for (int16_t i = x, e = x + w; i < e; i++)
      DrawVLine(i, y, h, c); // DrawVLine сам пометит страницы
    return;
  }

  // Для C_CLEAR однострочного случая помечаем здесь
  MARK_DIRTY(startPage);
}

// ---------------------------------------------------------------------------
// Вывод символа шрифта
// ---------------------------------------------------------------------------
static void m_putchar(int16_t x, int16_t y, uint8_t c, Color col, uint8_t sx,
                      uint8_t sy, const GFXfont *f) {
  const GFXglyph *g = &f->glyph[c - f->first];
  const uint8_t  *b = f->bitmap + g->bitmapOffset;
  uint8_t w = g->width, h = g->height, bits = 0, bit = 0;
  int8_t  xo = g->xOffset, yo = g->yOffset;

  // Быстрый путь: sx=1, sy=1, col=C_FILL (95% случаев)
  if (sx == 1 && sy == 1 && col == C_FILL) {
    for (uint8_t yy = 0; yy < h; yy++) {
      int16_t py = y + yo + yy;
      if (py < 0 || py >= LCD_HEIGHT)
        continue;

      uint8_t page = py >> 3;
      uint8_t mask = 1 << (py & 7);
      MARK_DIRTY(page);

      for (uint8_t xx = 0; xx < w; xx++, bits <<= 1) {
        if (!(bit++ & 7))
          bits = *b++;
        if (bits & 0x80) {
          int16_t px = x + xo + xx;
          if (px >= 0 && px < LCD_WIDTH)
            gFrameBuffer[page][px] |= mask;
        }
      }
    }
  } else {
    for (uint8_t yy = 0; yy < h; yy++) {
      for (uint8_t xx = 0; xx < w; xx++, bits <<= 1) {
        if (!(bit++ & 7))
          bits = *b++;
        if (bits & 0x80) {
          (sx == 1 && sy == 1)
              ? PutPixel(x + xo + xx, y + yo + yy, col)
              : FillRect(x + (xo + xx) * sx, y + (yo + yy) * sy, sx, sy, col);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Измерение текста / курсор
// ---------------------------------------------------------------------------
void charBounds(uint8_t c, int16_t *x, int16_t *y, int16_t *minx,
                int16_t *miny, int16_t *maxx, int16_t *maxy, uint8_t tsx,
                uint8_t tsy, bool wrap, const GFXfont *f) {
  if (c == '\n') {
    *x = 0;
    *y += tsy * f->yAdvance;
    return;
  }
  if (c == '\r' || c < f->first || c > f->last)
    return;

  const GFXglyph *g = &f->glyph[c - f->first];
  if (wrap && (*x + ((g->xOffset + g->width) * tsx) > LCD_WIDTH)) {
    *x = 0;
    *y += tsy * f->yAdvance;
  }

  int16_t x1 = *x + g->xOffset * tsx, y1 = *y + g->yOffset * tsy;
  int16_t x2 = x1 + g->width * tsx - 1, y2 = y1 + g->height * tsy - 1;
  if (x1 < *minx) *minx = x1;
  if (y1 < *miny) *miny = y1;
  if (x2 > *maxx) *maxx = x2;
  if (y2 > *maxy) *maxy = y2;
  *x += g->xAdvance * tsx;
}

static void getTextBounds(const char *s, int16_t x, int16_t y, int16_t *x1,
                          int16_t *y1, uint16_t *w, uint16_t *h,
                          const GFXfont *f) {
  int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;
  for (; *s; s++)
    charBounds(*s, &x, &y, &minx, &miny, &maxx, &maxy, 1, 1, 0, f);
  *x1 = maxx >= minx ? minx : x;
  *y1 = maxy >= miny ? miny : y;
  *w  = maxx >= minx ? maxx - minx + 1 : 0;
  *h  = maxy >= miny ? maxy - miny + 1 : 0;
}

inline static void write(uint8_t c, uint8_t tsx, uint8_t tsy, bool wrap,
                         Color col, const GFXfont *f) {
  if (c == '\n') {
    cursor.x = 0;
    cursor.y += tsy * f->yAdvance;
    return;
  }
  if (c == '\r' || c < f->first || c > f->last)
    return;

  GFXglyph *g = &f->glyph[c - f->first];
  if (g->width && g->height) {
    if (wrap && (cursor.x + tsx * (g->xOffset + g->width) > LCD_WIDTH)) {
      cursor.x = 0;
      cursor.y += tsy * f->yAdvance;
    }
    m_putchar(cursor.x, cursor.y, c, col, tsx, tsy, f);
  }
  cursor.x += g->xAdvance * tsx;
}

static void printStr(const GFXfont *f, uint8_t x, uint8_t y, Color col,
                     TextPos pos, const char *fmt, va_list args) {
  char buf[64];
  vsnprintf(buf, 64, fmt, args);

  if (pos == POS_L) {
    cursor.x = x;
    cursor.y = y;
    for (char *p = buf; *p; p++)
      write(*p, 1, 1, 1, col, f);
  } else {
    int16_t x1, y1;
    uint16_t w, h;
    getTextBounds(buf, x, y, &x1, &y1, &w, &h, f);
    cursor.x = pos == POS_C ? x - (w >> 1) : x - w;
    cursor.y = y;
    for (char *p = buf; *p; p++)
      write(*p, 1, 1, 1, col, f);
  }
}

// ---------------------------------------------------------------------------
// Публичные Print* функции
// ---------------------------------------------------------------------------
#define P(n, i)                                                                \
  void Print##n(uint8_t x, uint8_t y, const char *f, ...) {                   \
    va_list a;                                                                 \
    va_start(a, f);                                                            \
    printStr(fonts[i], x, y, C_FILL, POS_L, f, a);                            \
    va_end(a);                                                                 \
  }
#define PX(n, i)                                                               \
  void Print##n##Ex(uint8_t x, uint8_t y, TextPos p, Color c, const char *f,  \
                    ...) {                                                     \
    va_list a;                                                                 \
    va_start(a, f);                                                            \
    printStr(fonts[i], x, y, c, p, f, a);                                     \
    va_end(a);                                                                 \
  }

P(Small, 0)  PX(Small, 0)
P(Medium, 1) PX(Medium, 1)
P(BiggestDigits, 2) PX(BiggestDigits, 2)

void PrintSymbolsEx(uint8_t x, uint8_t y, TextPos p, Color c, const char *f,
                    ...) {
  va_list a;
  va_start(a, f);
  printStr(&Symbols, x, y, c, p, f, a);
  va_end(a);
}

void FSmall(uint8_t x, uint8_t y, TextPos a, uint32_t freq) {
  PrintSmallEx(x, y, a, C_FILL, "%u.%05u", freq / MHZ, freq % MHZ);
}

