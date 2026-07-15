#ifndef DRIVER_ST7565_H
#define DRIVER_ST7565_H

#include <stdbool.h>
#include <stdint.h>

#define FRAME_LINES 8

#define LCD_WIDTH 128
#define LCD_HEIGHT 64
#define LCD_XCENTER 64
#define LCD_YCENTER 32

extern uint8_t gFrameBuffer[FRAME_LINES][LCD_WIDTH];
static uint32_t gLastRender;
extern bool gRedrawScreen;
extern bool gLineChanged[FRAME_LINES]; // выставляется в graphics.c примитивами
// Флаг для подавления обновлений дисплея (например, при открытом шумодаве в FC)
extern bool gSuppressDisplayUpdates;

void ST7565_DrawLine(const unsigned int Column, const unsigned int Line,
                     const uint8_t *pBitmap, const unsigned int Size);
void ST7565_Blit(void);
void ST7565_BlitLine(unsigned line);
void ST7565_BlitStatusLine(void);
void ST7565_FillScreen(uint8_t Value);
void ST7565_Init(void);
void ST7565_FixInterfGlitch(void);
void ST7565_HardwareReset(void);
void ST7565_SelectColumnAndLine(uint8_t Column, uint8_t Line);
void ST7565_WriteByte(uint8_t Value);
void ST7565_SetContrast(uint8_t contrast);

#endif
