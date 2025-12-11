#include "backlight.h"
#include "../external/printf/printf.h"
#include "gpio.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_system.h"
#include "py32f071_ll_tim.h"

// this is decremented once every 500ms
uint16_t gBacklightCountdown_500ms = 0;
bool backlightOn;

void BACKLIGHT_InitHardware() {}

static void BACKLIGHT_Sound(void) {}

void BACKLIGHT_TurnOn(void) {
  backlightOn = true;

  BACKLIGHT_SetBrightness(255);

  gBacklightCountdown_500ms = 2 * 30;
}

void BACKLIGHT_TurnOff() {
  BACKLIGHT_SetBrightness(0);
  gBacklightCountdown_500ms = 0;
  backlightOn = false;
}

bool BACKLIGHT_IsOn() { return backlightOn; }

static uint8_t currentBrightness = 0;

void BACKLIGHT_SetBrightness(uint8_t brigtness) {
  if (currentBrightness == brigtness) {
    return;
  }

  if (0 == brigtness) {
    GPIO_TurnOffBacklight();
  } else {
    GPIO_TurnOnBacklight();
  }

  currentBrightness = brigtness;
}

uint8_t BACKLIGHT_GetBrightness(void) { return currentBrightness; }
