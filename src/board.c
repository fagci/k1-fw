#include "board.h"
#include "driver/backlight.h"
#include "driver/gpio.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_adc.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_bus.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_gpio.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_rcc.h"
#include <stdint.h>

void BOARD_GPIO_Init(void) {
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA   //
                          | LL_IOP_GRP1_PERIPH_GPIOB //
                          | LL_IOP_GRP1_PERIPH_GPIOC //
                          | LL_IOP_GRP1_PERIPH_GPIOF //
  );

  LL_GPIO_InitTypeDef InitStruct;
  LL_GPIO_StructInit(&InitStruct);
  InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  InitStruct.Pull = LL_GPIO_PULL_UP;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;

  // ---------------------
  // Input pins

  InitStruct.Mode = LL_GPIO_MODE_INPUT;

  // Keypad rows: PB15:12
  InitStruct.Pin =
      LL_GPIO_PIN_15 | LL_GPIO_PIN_14 | LL_GPIO_PIN_13 | LL_GPIO_PIN_12;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // PTT: PB10
  InitStruct.Pin = LL_GPIO_PIN_10;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // -----------------------
  //  Output pins

  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_6); // LCD A0
  LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_2); // LCD CS

  InitStruct.Mode = LL_GPIO_MODE_OUTPUT;

  // Keypad cols: PB6:3
  InitStruct.Pin =
      LL_GPIO_PIN_6 | LL_GPIO_PIN_5 | LL_GPIO_PIN_4 | LL_GPIO_PIN_3;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // Audio PA: PA8
  // LCD A0: PA6
  // SPI flash CS: PA3
  InitStruct.Pin = LL_GPIO_PIN_8 | LL_GPIO_PIN_6 | LL_GPIO_PIN_3;
  LL_GPIO_Init(GPIOA, &InitStruct);

  // BK4829 SCK: B8
  // BK4829 SDA: B9
  // LCD CS: PB2
  InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8 | LL_GPIO_PIN_2;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // Flashlight: PC13
  InitStruct.Pin = LL_GPIO_PIN_13;
  LL_GPIO_Init(GPIOC, &InitStruct);

  // BK1080 SCK: PF5
  // BK1080 SDA: PF6
  InitStruct.Pin = LL_GPIO_PIN_6 | LL_GPIO_PIN_5;
  LL_GPIO_Init(GPIOF, &InitStruct);

  // Backlight: PF8
  // BK4819 CS: PF9
  InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8;
  LL_GPIO_Init(GPIOF, &InitStruct);
}

void BOARD_ADC_Init(void) {
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);
  LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_0 | LL_GPIO_PIN_1, LL_GPIO_MODE_ANALOG);

  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);
  LL_RCC_SetADCClockSource(LL_RCC_ADC_CLKSOURCE_PCLK_DIV4);

  LL_ADC_SetCommonPathInternalCh(ADC1_COMMON, LL_ADC_PATH_INTERNAL_NONE);
  LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
  LL_ADC_SetDataAlignment(ADC1, LL_ADC_DATA_ALIGN_RIGHT);
  LL_ADC_SetSequencersScanMode(ADC1, LL_ADC_SEQ_SCAN_DISABLE);
  LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
  LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_SINGLE);
  LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);
  LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_DISABLE);
  LL_ADC_REG_SetSequencerDiscont(ADC1, LL_ADC_REG_SEQ_DISCONT_DISABLE);
  LL_ADC_REG_SetSequencerDiscont(ADC1, LL_ADC_REG_SEQ_DISCONT_DISABLE);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_8);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_8,
                                LL_ADC_SAMPLINGTIME_41CYCLES_5);

  LL_ADC_StartCalibration(ADC1);
  while (LL_ADC_IsCalibrationOnGoing(ADC1))
    ;

  LL_ADC_Enable(ADC1);
}

void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent) {
  LL_ADC_REG_StartConversionSWStart(ADC1);
  while (!LL_ADC_IsActiveFlag_EOS(ADC1))
    ;
  LL_ADC_ClearFlag_JEOS(ADC1);

  *pVoltage = LL_ADC_REG_ReadConversionData12(ADC1);
  *pCurrent = 0;
}

void BOARD_Init(void) {
  BOARD_GPIO_Init();
  BACKLIGHT_InitHardware();
  BOARD_ADC_Init();
  PY25Q16_Init();
  ST7565_Init();
}

void BOARD_FlashlightToggle() { GPIO_TogglePin(GPIO_PIN_FLASHLIGHT); }
