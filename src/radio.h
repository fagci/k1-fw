#ifndef RADIO_H
#define RADIO_H

#include "driver/bk4829.h"
#include "inc/band.h"
#include "inc/vfo.h"
#include <stdbool.h>
#include <stdint.h>

#define SQL_DELAY 150  // Увеличено с 90 — реже опрашиваем регистры, меньше SPI шума

extern const char *PARAM_NAMES[];
extern const char *TX_STATE_NAMES[7];
extern const char *FLT_BOUND_NAMES[2];
extern const char *BW_NAMES_BK4819[10];
extern const char *BW_NAMES_SI47XX[7];
extern const char *BW_NAMES_SI47XX_SSB[6];
extern const char *SQ_TYPE_NAMES[4];
extern const char *MOD_NAMES_BK4819[8];
extern const char *RADIO_NAMES[3];

extern const uint16_t StepFrequencyTable[15];

extern Band gCurrentBand;
extern bool gShowAllRSSI;
extern bool gMonitorMode;
extern ExtendedVFOContext *vfo;
extern VFOContext *ctx;

// New functions for multi-VFO and multiwatch support
void RADIO_InitState(RadioState *state, uint8_t num_vfos);
bool RADIO_SwitchVFO(RadioState *state, uint8_t vfo_index);
void RADIO_LoadVFOFromStorage(RadioState *state, uint8_t vfo_index,
                              const VFO *storage);
void RADIO_SaveVFOToStorage(const RadioState *state, uint8_t vfo_index,
                            VFO *storage);
void RADIO_LoadChannelToVFO(RadioState *state, uint8_t vfo_index,
                            uint16_t channel_index);
bool RADIO_NextChannel(bool next);
bool RADIO_SaveCurrentVFO(RadioState *state);
bool RADIO_SaveAllVFOs(RadioState *state);
void RADIO_ToggleMultiwatch(RadioState *state, bool enable);
void RADIO_UpdateMultiwatch(RadioState *state);
bool RADIO_ToggleVFOMode(RadioState *state, uint8_t vfo_index);

// Инициализация
void RADIO_Init(VFOContext *ctx, Radio radio_type);

// Установка параметра
void RADIO_SetParam(VFOContext *ctx, ParamType param, uint32_t value,
                    bool save_to_eeprom);

void RADIO_CheckAndSaveVFO(RadioState *state);

// Применение настроек
void RADIO_ApplySettings(VFOContext *ctx);

// Применение одного из 4 полей AF response (gSettings.af_rx_300/af_rx_3k/
// af_tx_300/af_tx_3k) к железу — единая точка для radio.c/system.c/settings.c
void RADIO_ApplyAFResponse(bool tx, bool is3k);
void RADIO_ApplyAllAFResponse(void);

// Проверка поддержки параметра в текущем диапазоне
bool RADIO_IsParamValid(VFOContext *ctx, ParamType param, uint32_t value);

uint32_t RADIO_GetParam(const VFOContext *ctx, ParamType param);
bool RADIO_AdjustParam(VFOContext *ctx, ParamType param, uint32_t inc,
                       bool save_to_eeprom);
bool RADIO_IncDecParam(VFOContext *ctx, ParamType param, bool inc,
                       bool save_to_eeprom);
void RADIO_LoadVFOs(RadioState *state);

void RADIO_EnableAudioRouting(RadioState *state, bool enable);
void RADIO_UpdateAudioRouting(RadioState *state);

void RADIO_ToggleTX(VFOContext *ctx, bool on);
bool RADIO_IsSSB(const VFOContext *ctx);
const char *RADIO_GetParamValueString(const VFOContext *ctx, ParamType param);

uint8_t RADIO_GetCurrentVFONumber(const RadioState *state);
ExtendedVFOContext *RADIO_GetCurrentVFO(RadioState *state);
const ExtendedVFOContext *RADIO_GetCurrentVFOConst(const RadioState *state);

void RADIO_SwitchAudioToVFO(RadioState *state, uint8_t vfo_index);
void RADIO_UpdateSquelch(RadioState *state);

uint16_t RADIO_GetRSSI(const VFOContext *ctx);
uint8_t RADIO_GetSNR(const VFOContext *ctx);
uint8_t RADIO_GetNoise(const VFOContext *ctx);
uint8_t RADIO_GetGlitch(const VFOContext *ctx);

void RADIO_FastSquelchUpdate();
void RADIO_SlowRSSIUpdate();

void RADIO_MuteAudioNow(RadioState *state);
void RADIO_UnmuteAudioNow(RadioState *state);

const char *RADIO_GetParamName(ParamType p);

extern RadioState *gRadioState;
extern ExtendedVFOContext *vfo;
extern VFOContext *ctx;

#endif // RADIO_H
