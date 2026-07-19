#include "spectrum.h"
#include "../board.h"
#include "../driver/uart.h"
#include "../helper/measurements.h"
#include "components.h"
#include "graphics.h"
#include <stdint.h>
#include <string.h>

#define MAX_POINTS 128

uint8_t SPECTRUM_Y = 8;
uint8_t SPECTRUM_H = 44;
GraphMeasurement graphMeasurement = GRAPH_RSSI;

static uint8_t S_BOTTOM;
static uint16_t rssiHistory[MAX_POINTS] = {0};
static uint8_t noiseHistory[MAX_POINTS] = {0};
static uint8_t glitchHistory[MAX_POINTS] = {0};
static uint8_t x = 0;
static uint8_t ox = UINT8_MAX;
static uint8_t filledPoints;
static Band *range;
static uint16_t step;

// fallback-границы в сырых единицах RSSI (≈ -120..-60 dBm)
#define SP_RSSI_MIN 30
#define SP_RSSI_MAX 200

// RSSI → высота бара в пикселях [0..SPECTRUM_H], прямой линейный маппинг
static uint8_t rssi2Y(uint16_t rssi, VMinMax v) {
  if (rssi <= v.vMin)
    return 0;
  if (rssi >= v.vMax)
    return SPECTRUM_H;
  return (uint8_t)((uint32_t)(rssi - v.vMin) * SPECTRUM_H / (v.vMax - v.vMin));
}

// ────────────────────────────────────────────────────────────────────

static void drawTicks(uint8_t y, uint32_t fs, uint32_t fe, uint32_t div,
                      uint8_t h) {
  for (uint32_t f = fs - (fs % div) + div; f < fe; f += div)
    DrawVLine(SP_F2X(f), y, h, C_FILL);
}

void UI_DrawTicks(uint8_t y, const Band *band) {
  uint32_t fs = band->start, fe = band->end, bw = fe - fs;
  for (uint32_t p = 100000000; p >= 10; p /= 10) {
    if (p < bw) {
      drawTicks(y, fs, fe, p / 2, 2);
      drawTicks(y, fs, fe, p, 3);
      return;
    }
  }
}

// ────────────────────────────────────────────────────────────────────

static uint8_t visited[MAX_POINTS / 8 + 1] = {0};
static uint8_t spPrevXc = 0;
static uint32_t spPeakF = 0;
static uint16_t spPeakRssi = 0;
static uint32_t spPeakFDisp = 0;
static uint16_t spPeakRssiDisp = 0;

static VMinMax autoLevel = {SP_RSSI_MIN, SP_RSSI_MAX};

// Минимальный span в "сырых" единицах текущего канала графика. По умолчанию
// откалиброван под RSSI (40 ≈ 20дБ) — для остальных параметров (noise,
// AGC RSSI, AFC и т.п.), у которых совсем другой естественный диапазон,
// вызывающая сторона (analyser.c) должна выставить подходящее значение через
// SP_SetAutoLevelMinSpan(), иначе автомасштаб либо переусердствует с зумом,
// либо наоборот не даст увеличить малую реальную вариацию.
static uint16_t autoLevelMinSpan = 40;

void SP_SetAutoLevelMinSpan(uint16_t span) {
  autoLevelMinSpan = span ? span : 1;
}

// Пересчитывается в конце каждого свипа (вызов SP_Begin).
// floor: минимальный RSSI истории → 2px снизу.
static void computeAutoLevel(void) {
  if (filledPoints < 2)
    return;

  uint16_t rMin = rssiHistory[0], rMax = rssiHistory[0];
  for (uint8_t i = 1; i < filledPoints; i++) {
    if (rssiHistory[i] < rMin)
      rMin = rssiHistory[i];
    if (rssiHistory[i] > rMax)
      rMax = rssiHistory[i];
  }

  uint16_t span = rMax > rMin ? rMax - rMin : 0;
  if (span < autoLevelMinSpan)
    span = autoLevelMinSpan;

  // floor на 2px: vMin = rMin - 2*span/SPECTRUM_H
  uint16_t margin = 2 * span / SPECTRUM_H;
  uint16_t vMin = rMin > margin ? rMin - margin : 0;
  autoLevel = (VMinMax){.vMin = vMin, .vMax = vMin + span};
}

VMinMax SP_GetAutoLevel(void) { return autoLevel; }

void SP_ResetHistory(void) {
  filledPoints = 0;
  memset(rssiHistory, 0, sizeof(rssiHistory));
  memset(noiseHistory, 0, sizeof(noiseHistory));
  memset(glitchHistory, 0, sizeof(glitchHistory));
  memset(visited, 0, sizeof(visited));
}

void SP_Begin(void) {
  computeAutoLevel();
  x = 0;
  ox = UINT8_MAX;
  spPrevXc = 0;
  spPeakFDisp = spPeakF;
  spPeakRssiDisp = spPeakRssi;
  spPeakF = 0;
  spPeakRssi = 0;
  memset(visited, 0, sizeof(visited));
}

void SP_Init(Band *b) {
  S_BOTTOM = SPECTRUM_Y + SPECTRUM_H;
  range = b;
  step = StepFrequencyTable[b->step];
  SP_ResetHistory();
  SP_Begin();
}

// ────────────────────────────────────────────────────────────────────

uint8_t SP_F2X(uint32_t f) {
  if (f <= range->start)
    return 0;
  if (f >= range->end)
    return MAX_POINTS - 1;
  uint32_t delta = f - range->start;
  uint32_t aRange = range->end - range->start;
  return (uint8_t)(((uint64_t)delta * (MAX_POINTS - 1) + aRange / 2) / aRange);
}

uint32_t SP_X2F(uint8_t xi) {
  if (xi >= MAX_POINTS)
    xi = MAX_POINTS - 1;
  uint32_t aRange = range->end - range->start;
  return range->start +
         (uint32_t)(((uint64_t)xi * aRange + (MAX_POINTS - 1) / 2) /
                    (MAX_POINTS - 1));
}

// ────────────────────────────────────────────────────────────────────

void SP_AddPoint(const Measurement *msm) {
  uint8_t xc = SP_F2X(msm->f);
  bool isLast = (msm->f + step >= range->end);

  // Voronoi-границы: вычисляем next_xc только если нужно
  int16_t ixs, ixe;
  if (xc == 0) {
    ixs = 0;
    ixe = isLast ? (MAX_POINTS - 1) : (int16_t)(xc + SP_F2X(msm->f + step)) / 2;
  } else if (isLast) {
    ixs = (int16_t)(spPrevXc + xc) / 2 + 1;
    ixe = MAX_POINTS - 1;
  } else {
    ixs = (int16_t)(spPrevXc + xc) / 2 + 1;
    ixe = (int16_t)(xc + SP_F2X(msm->f + step)) / 2;
  }

  // гарантируем минимум сам xc (Voronoi может выродиться в пустой диапазон)
  if (ixs > xc)
    ixs = xc;
  if (ixe < xc)
    ixe = xc;
  if (ixs < 0)
    ixs = 0;
  if (ixe > MAX_POINTS - 1)
    ixe = MAX_POINTS - 1;

  // инкрементный трекинг байта/бита вместо деления на каждой итерации
  uint8_t xi = (uint8_t)ixs;
  uint8_t byte = xi >> 3;
  uint8_t bit = xi & 7;
  uint8_t mask = 1 << bit;

  for (; xi <= (uint8_t)ixe; ++xi) {
    if ((visited[byte] & mask) == 0) {
      rssiHistory[xi] = msm->rssi;
      noiseHistory[xi] = msm->noise;
      glitchHistory[xi] = msm->glitch;
      visited[byte] |= mask;
    } else if (msm->rssi > rssiHistory[xi]) {
      rssiHistory[xi] = msm->rssi;
      noiseHistory[xi] = msm->noise;
      glitchHistory[xi] = msm->glitch;
    }
    mask <<= 1;
    if (!mask) {
      mask = 1;
      ++byte;
    } // переход к следующему байту
  }

  uint8_t end = (uint8_t)ixe + 1;
  if (end > filledPoints)
    filledPoints = end;

  spPrevXc = xc;

  if (msm->rssi > spPeakRssi) {
    spPeakRssi = msm->rssi;
    spPeakF = msm->f;
  }
}

// ────────────────────────────────────────────────────────────────────

VMinMax SP_GetMinMax(void) { return autoLevel; }

VMinMax SP_GetGraphMinMax(void) {
  switch (graphMeasurement) {
  case GRAPH_NOISE:
    return (VMinMax){.vMin = 0, .vMax = 128};
  case GRAPH_GLITCH:
    return (VMinMax){.vMin = 0, .vMax = 255};
  default:
    return SP_GetMinMax();
  }
}

// ────────────────────────────────────────────────────────────────────

uint8_t SP_FindPeakX(void) {
  uint32_t f = SP_GetPeakF();
  return f ? SP_F2X(f) : MAX_POINTS / 2;
}
uint32_t SP_GetPeakF(void) { return spPeakFDisp ? spPeakFDisp : spPeakF; }
uint16_t SP_GetPeakRssi(void) {
  return spPeakRssiDisp ? spPeakRssiDisp : spPeakRssi;
}

void SP_RenderMarker(uint8_t mx, VMinMax v) {
  if (mx >= filledPoints)
    return;
  uint8_t barH = rssi2Y(rssiHistory[mx], v);
  uint8_t top = S_BOTTOM - barH;

  for (uint8_t y = SPECTRUM_Y + 3; y < top; y += 2)
    PutPixel(mx, y, C_INVERT);

  FillRect(mx - 2, SPECTRUM_Y, 5, 1, C_INVERT);
  FillRect(mx - 1, SPECTRUM_Y + 1, 3, 1, C_INVERT);
  PutPixel(mx, SPECTRUM_Y + 2, C_INVERT);
}

// сетка, шаг в единицах RSSI
void SP_RenderDbmGrid(VMinMax v, uint16_t stepRssi) {
  for (uint16_t rssi = (v.vMin / stepRssi + 1) * stepRssi; rssi < v.vMax;
       rssi += stepRssi) {
    uint8_t py = S_BOTTOM - rssi2Y(rssi, v);
    for (uint8_t xi = 0; xi < MAX_POINTS; xi += 4)
      DrawHLine(xi, py, 2, C_FILL);
    PrintSmallEx(0, py - 1, POS_L, C_FILL, "%u", rssi);
  }
}

// ────────────────────────────────────────────────────────────────────

void SP_Render(const Band *p, VMinMax v) {
  if (p)
    UI_DrawTicks(S_BOTTOM, p);
  DrawHLine(0, S_BOTTOM, MAX_POINTS, C_FILL);

  if (graphMeasurement == GRAPH_RSSI) {
    for (uint8_t i = 0; i < filledPoints; ++i) {
      uint8_t yVal = rssi2Y(rssiHistory[i], v);
      DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
    }
  } else {
    VMinMax rv = SP_GetGraphMinMax();
    for (uint8_t i = 0; i < filledPoints; ++i) {
      uint16_t val = (graphMeasurement == GRAPH_NOISE) ? noiseHistory[i]
                                                       : glitchHistory[i];
      uint8_t yVal = ConvertDomain(val, rv.vMin, rv.vMax, 0, SPECTRUM_H);
      DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
    }
  }
}

// ────────────────────────────────────────────────────────────────────

void SP_RenderArrow(uint32_t f) {
  uint8_t cx = SP_F2X(f);
  DrawVLine(cx, SPECTRUM_Y + SPECTRUM_H + 1, 1, C_FILL);
  FillRect(cx - 1, SPECTRUM_Y + SPECTRUM_H + 2, 3, 1, C_FILL);
  FillRect(cx - 2, SPECTRUM_Y + SPECTRUM_H + 3, 5, 1, C_FILL);
}

void SP_RenderRssi(uint16_t rssi, char *text, bool top, VMinMax v) {
  uint8_t yVal = rssi2Y(rssi, v);
  DrawHLine(0, S_BOTTOM - yVal, filledPoints, C_FILL);
  PrintSmallEx(0, S_BOTTOM - yVal + (top ? -2 : 6), POS_L, C_FILL, "%s %u",
               text, rssi);
}

void SP_RenderLine(uint16_t value, VMinMax v) {
  uint8_t yVal;
  if (graphMeasurement == GRAPH_RSSI) {
    yVal = rssi2Y(value, v);
  } else {
    VMinMax rv = SP_GetGraphMinMax(); // те же границы что у SP_Render для N/G
    yVal = ConvertDomain(value, rv.vMin, rv.vMax, 0, SPECTRUM_H);
  }
  DrawHLine(0, S_BOTTOM - yVal, filledPoints, C_INVERT);
}

void SP_RenderPoint(Measurement *m, uint8_t i, uint8_t n, Band *b, VMinMax r,
                    Color c) {
  uint8_t yVal = rssi2Y(m->rssi, r);
  PutPixel(i, S_BOTTOM - yVal, c);
}

// ────────────────────────────────────────────────────────────────────

uint16_t SP_GetNoiseFloor(void) {
  if (filledPoints < 10)
    return 0;
  uint16_t temp[MAX_POINTS];
  memcpy(temp, rssiHistory, sizeof(uint16_t) * filledPoints);

  // нужен только p25 — частичная сортировка выбором до target
  uint8_t target = filledPoints / 4;
  for (uint8_t i = 0; i <= target; i++) {
    uint8_t minIdx = i;
    for (uint8_t j = i + 1; j < filledPoints; j++)
      if (temp[j] < temp[minIdx])
        minIdx = j;
    uint16_t t = temp[i];
    temp[i] = temp[minIdx];
    temp[minIdx] = t;
  }
  return temp[target];
}

uint16_t SP_GetRssiMax(void) { return Max(rssiHistory, filledPoints); }
uint16_t SP_GetPointRSSI(uint8_t i) { return rssiHistory[i]; }
uint16_t SP_GetPointNoise(uint8_t i) { return noiseHistory[i]; }
uint16_t SP_GetPointGlitch(uint8_t i) { return glitchHistory[i]; }
uint16_t SP_GetLastGraphValue(void) { return rssiHistory[MAX_POINTS - 1]; }

// ────────────────────────────────────────────────────────────────────

void SP_RenderGraph(uint16_t min, uint16_t max) {
  const VMinMax v = {.vMin = min, .vMax = max};
  S_BOTTOM = SPECTRUM_Y + SPECTRUM_H;
  FillRect(0, SPECTRUM_Y, LCD_WIDTH, SPECTRUM_H, C_CLEAR);
  uint8_t oVal = ConvertDomain(rssiHistory[0], v.vMin, v.vMax, 0, SPECTRUM_H);
  const uint8_t centerY = SPECTRUM_Y + SPECTRUM_H / 2;
  if (graphMeasurement == GRAPH_TX) {
    for (uint8_t i = 0; i < MAX_POINTS; ++i) {
      uint8_t amp =
          ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H / 2);
      DrawVLine(i, centerY - amp, amp * 2 + 1, C_FILL);
    }
  } else {
    for (uint8_t i = 1; i < MAX_POINTS; ++i) {
      uint8_t yVal =
          ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H);
      DrawLine(i - 1, S_BOTTOM - oVal, i, S_BOTTOM - yVal, C_FILL);
      oVal = yVal;
    }
  }
  DrawHLine(0, SPECTRUM_Y, LCD_WIDTH, C_FILL);
  DrawHLine(0, S_BOTTOM, LCD_WIDTH, C_FILL);
  for (uint8_t xi = 0; xi < LCD_WIDTH; xi += 4)
    DrawHLine(xi, centerY, 2, C_FILL);
}

void SP_NextGraphUnit(bool next) {
  graphMeasurement = IncDecU(graphMeasurement, 0, GRAPH_COUNT, next);
}

void SP_AddGraphPoint(const Measurement *msm) {
  uint16_t v = msm->rssi;
  switch (graphMeasurement) {
  case GRAPH_NOISE:
    v = msm->noise;
    break;
  case GRAPH_GLITCH:
    v = msm->glitch;
    break;
  case GRAPH_SNR:
    v = msm->snr;
    break;
  case GRAPH_TX:
    v = BK4819_GetVoiceAmplitude();
    break;
  default:
    break;
  }
  rssiHistory[MAX_POINTS - 1] = v;
  filledPoints = MAX_POINTS;
}

// ────────────────────────────────────────────────────────────────────

static void shiftEx(uint16_t *history, uint16_t n, int16_t shift) {
  if (shift == 0 || (shift >= n) || (shift <= -(int16_t)n))
    return;
  if (shift > 0) {
    memmove(history + shift, history, (n - shift) * sizeof(uint16_t));
    memset(history, 0, shift * sizeof(uint16_t));
  } else {
    uint16_t s = (uint16_t)(-shift);
    memmove(history, history + s, (n - s) * sizeof(uint16_t));
    memset(history + n - s, 0, s * sizeof(uint16_t));
  }
}

void SP_Shift(int16_t n) { shiftEx(rssiHistory, MAX_POINTS, n); }
void SP_ShiftGraph(int16_t n) { shiftEx(rssiHistory, MAX_POINTS, n); }

// ────────────────────────────────────────────────────────────────────

static uint8_t curX = MAX_POINTS / 2;
static uint8_t curSbWidth = 16;

void CUR_Render(void) {
  for (uint8_t y = SPECTRUM_Y + 10; y < S_BOTTOM; y += 4) {
    DrawVLine(curX - curSbWidth, y, 2, C_INVERT);
    DrawVLine(curX + curSbWidth, y, 2, C_INVERT);
  }
  for (uint8_t y = SPECTRUM_Y + 10; y < S_BOTTOM; y += 2)
    DrawVLine(curX, y, 1, C_INVERT);
}

bool CUR_Move(bool up) {
  if (up) {
    if (curX + curSbWidth < MAX_POINTS - 4) {
      curX += 4;
      return true;
    }
  } else {
    if (curX - curSbWidth >= 4) {
      curX -= 4;
      return true;
    }
  }
  return false;
}

bool CUR_Size(bool up) {
  if (up) {
    if (curX + curSbWidth < MAX_POINTS - 1 && curX - curSbWidth > 0) {
      curSbWidth++;
      return true;
    }
  } else {
    if (curSbWidth > 1) {
      curSbWidth--;
      return true;
    }
  }
  return false;
}

Band CUR_GetRange(Band *p, uint32_t step) {
  Band r = *p;
  r.start = RoundToStep(SP_X2F(curX - curSbWidth), step);
  r.end = RoundToStep(SP_X2F(curX + curSbWidth), step);
  return r;
}

uint32_t CUR_GetCenterF(uint32_t step) {
  return RoundToStep(SP_X2F(curX), step);
}

void CUR_Reset(void) {
  curX = MAX_POINTS / 2;
  curSbWidth = 16;
}

void CUR_SetPosByFreq(uint32_t f) { curX = SP_F2X(f); }
