#include "crc.h"

uint16_t CRC_Calculate(const void *pBuffer, uint16_t Size) {
  const uint8_t *pData = (const uint8_t *)pBuffer;
  uint16_t i, Crc;

  Crc = 0;
  for (i = 0; i < Size; i++) {
    Crc ^= (pData[i] << 8);

    for (int j = 0; j < 8; j++) {
      // Check bit [15]
      if (Crc >> 15) {
        Crc = (Crc << 1) ^ 0x1021;
      } else {
        Crc = Crc << 1;
      }
    }
  }

  return Crc;
}
