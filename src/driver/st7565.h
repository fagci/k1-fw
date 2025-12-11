#ifndef DRIVER_ST7565_H
#define DRIVER_ST7565_H

#include <stdbool.h>
#include <stdint.h>

#define LCD_WIDTH       128
#define LCD_HEIGHT       64
#define FRAME_LINES 8

extern uint8_t gStatusLine[LCD_WIDTH];
extern uint8_t gFrameBuffer[FRAME_LINES][LCD_WIDTH];

void ST7565_DrawLine(const unsigned int Column, const unsigned int Line, const uint8_t *pBitmap, const unsigned int Size);
void ST7565_BlitFullScreen(void);
void ST7565_BlitLine(unsigned line);
void ST7565_BlitStatusLine(void);
void ST7565_FillScreen(uint8_t Value);
void ST7565_Init(void);
void ST7565_FixInterfGlitch(void);
void ST7565_HardwareReset(void);
void ST7565_SelectColumnAndLine(uint8_t Column, uint8_t Line);
void ST7565_WriteByte(uint8_t Value);

#endif
