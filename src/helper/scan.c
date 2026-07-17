#include "scan.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/lootlist.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/spectrum.h"
#include "bands.h"
#include "measurements.h"

#define GARBAGE_FREQ_STEP 650000U
#define SOFT_SQ_HEADROOM 25 // % смягчения аппаратных порогов
#define STE_DEBOUNCE_MS 250 // окно подавления STE-хвоста

// Двухфазное обнаружение (по эмпирическим замерам). RSSI "усаживается" на
// реальный канал быстро (~scan.warmupUs, ~2.2мс), но сам по себе не
// отличает сигнал от шума/наводки — годится только как дешёвый
// предфильтр "стоит ли досиживать". Noise усаживается медленнее (нужно
// добрать время примерно до 5мс), зато селективен — отражает попадание
// сигнала именно в полосу RF-фильтра, поэтому окончательное решение
// принимает он, а не RSSI.
#define NOISE_CONFIRM_EXTRA_US 2800 // добор с ~2200 до ~5000мкс

static uint32_t preScanF = 0;

// GetSqlPreset() читает файл пресета с флеша (Storage_Load) при каждом
// вызове — недопустимо дорого на каждый шаг свипа. Уровень шумодава и
// VHF/UHF-диапазон меняются редко, поэтому кэшируем результат и
// перечитываем только когда один из них реально изменился.
static SquelchPreset cachedSq;
static uint8_t cachedSqLevel = 0xFF; // заведомо невалидный — форсирует первую загрузку
static bool cachedSqIsUHF = false;

static SquelchPreset GetSqlPresetCached(uint8_t level, uint32_t freq) {
  bool isUHF = freq >= SETTINGS_GetFilterBound();
  if (level != cachedSqLevel || isUHF != cachedSqIsUHF) {
    cachedSq = GetSqlPreset(level, freq);
    cachedSqLevel = level;
    cachedSqIsUHF = isUHF;
  }
  return cachedSq;
}

static ScanContext scan = {
    .state = SCAN_STATE_IDLE,
    .mode = SCAN_MODE_SINGLE,
    .warmupUs = 2500,
    .isOpen = false,
    .cmdRangeActive = false,
    .cmdCtx = NULL,
};

static SCMD_Context cmdctx;
static uint32_t sqReopenAt = 0;

// Хелпер для PLL_VCO бита REG_30. Чтение REG_30 в драйвере bk4829.c кэшируется
// глобально (см. reg30state), поэтому повторные reads бесплатны. Писать —
// дорого, драйвер write не фильтрует. Поэтому пропускаем write, если бит уже в
// нужном состоянии.
static void Reg30_SetPllVco(bool on) {
  uint16_t cur = BK4819_ReadRegister(BK4819_REG_30);
  uint16_t nv = on ? (cur | BK4819_REG_30_ENABLE_PLL_VCO)
                   : (cur & ~BK4819_REG_30_ENABLE_PLL_VCO);
  if (nv != cur)
    BK4819_WriteRegister(BK4819_REG_30, nv);
}

const char *SCAN_MODE_NAMES[] = {
    [SCAN_MODE_NONE] = "None",         [SCAN_MODE_SINGLE] = "VFO",
    [SCAN_MODE_FREQUENCY] = "Scan",    [SCAN_MODE_CHANNEL] = "CH Scan",
    [SCAN_MODE_ANALYSER] = "Analyser", [SCAN_MODE_MULTIWATCH] = "MultiWatch",
};

const char *SCAN_STATE_NAMES[] = {
    [SCAN_STATE_IDLE] = "Idle",
    [SCAN_STATE_TUNING] = "Tuning",
    [SCAN_STATE_CHECKING] = "Checking",
    [SCAN_STATE_LISTENING] = "Listening",
};

// ============================================================================

static void STE_StartGate(void) {
  if (ctx->code.type != 0)
    sqReopenAt = Now() + STE_DEBOUNCE_MS;
}

static bool IsSqOpenGated(void) {
  return BK4819_IsSquelchOpen() && (Now() >= sqReopenAt);
}

static bool IsSkippable(uint32_t f) {
  if (gSettings.skipGarbageFrequencies && (f % GARBAGE_FREQ_STEP == 0))
    return true;
  Loot *l = LOOT_Get(f);
  return l && (l->blacklist || l->whitelist);
}

static void ChangeState(ScanState s) {
  if (scan.state != s) {
    scan.state = s;
    scan.stateEnteredAt = Now();
  }
}

static uint32_t ElapsedMs(void) { return Now() - scan.stateEnteredAt; }

static void UpdateCPS(void) {
  uint32_t now = Now();
  uint32_t elapsed = now - scan.lastCpsTime;
  if (elapsed >= 1000) {
    scan.currentCps = (scan.scanCycles * 1000) / elapsed;
    scan.lastCpsTime = now;
    scan.scanCycles = 0;
  }
}

static void ApplyBandSettings(void) {
  vfo->msm.f = gCurrentBand.start;
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, vfo->msm.f, false);
  RADIO_SetParam(ctx, PARAM_STEP, gCurrentBand.step, false);
  RADIO_ApplySettings(ctx);
  SP_Init(&gCurrentBand);
  if (gLastActiveLoot && !BANDS_InRange(gLastActiveLoot->f, &gCurrentBand))
    gLastActiveLoot = NULL;
}

static void BeginScanRange(uint32_t start, uint32_t end, uint16_t step) {
  scan.startF = start;
  scan.endF = end;
  scan.currentF = start;
  scan.stepF = step;
  scan.cmdRangeActive = true;
  ChangeState(SCAN_STATE_TUNING);
}

static void UpdateBandAndRestart(void) {
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER)
    BeginScanRange(gCurrentBand.start, gCurrentBand.end,
                   StepFrequencyTable[gCurrentBand.step]);
}

// ============================================================================

static void ApplyCommand(SCMD_Command *cmd) {
  if (!cmd)
    return;

  switch (cmd->type) {
  case SCMD_CHANNEL:
    BeginScanRange(cmd->start, cmd->start, 0);
    return;
  case SCMD_RANGE:
    BeginScanRange(cmd->start, cmd->end, cmd->step);
    return;
  case SCMD_PAUSE:
    SYSTICK_DelayMs(cmd->dwell_ms); // TODO: неблокирующая задержка
    break;
  default:
    break;
  }
  // MARKER, PAUSE (fall-through), JUMP, прочие — просто переходим дальше
  if (!SCMD_Advance(scan.cmdCtx))
    SCMD_Rewind(scan.cmdCtx);
}

static void HandleEndOfRange(void) {
  if (scan.cmdCtx) {
    if (!SCMD_Advance(scan.cmdCtx))
      SCMD_Rewind(scan.cmdCtx);
    scan.cmdRangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
  } else {
    scan.currentF = scan.startF;
    ChangeState(SCAN_STATE_TUNING);
    SP_Begin();
  }
  gRedrawScreen = true;
}

// ============================================================================

static void HandleStateIdle(void) {
  if (!scan.cmdCtx || scan.cmdRangeActive)
    return;
  SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
  if (cmd)
    ApplyCommand(cmd);
}

static void HandleStateTuning(void) {
  if (scan.stepF == 0) {
    if (IsSkippable(scan.currentF)) {
      HandleEndOfRange();
      return;
    }
  } else {
    uint32_t skipFrom = scan.currentF;
    while (scan.currentF <= scan.endF && IsSkippable(scan.currentF))
      scan.currentF += scan.stepF;

    // Пропущенные частоты не измеряются, а SP_Begin() не чистит историю
    // графика — без этого их пиксели застывают навсегда. Продлеваем
    // последний реальный замер до начала пропуска
    if (scan.currentF != skipFrom && skipFrom > scan.startF) {
      Measurement bridge = scan.measurement;
      bridge.f = skipFrom;
      SP_AddPoint(&bridge);
    }
  }

  if (scan.currentF > scan.endF) {
    HandleEndOfRange();
    return;
  }

  RADIO_MuteAudioNow(gRadioState);

  // Включаем VCO перед перестройкой (мог быть выключен после прошлого замера)
  Reg30_SetPllVco(true);

  // precise=false: только импульс ENABLE_VCO_CALIB вместо полного сброса
  // REG_30, без паузы на полную рекалибровку. Полный сброс (precise=true)
  // глушит на время рестабилизации весь аналоговый тракт, а не только
  // калибровку VCO — фиксированное окно замера (warmupUs) с этим не
  // справляется, из-за чего замер на этом шаге может ложно не увидеть
  // даже сильный сигнал. Поэтому здесь всегда precise=false; см.
  // HandleStateListening/SCAN_Next про попытку форсировать true.
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);

  SYSTICK_DelayUs(scan.warmupUs);

  scan.measurement.rssi = RADIO_GetRSSI(ctx);
  scan.measurement.noise = BK4819_GetNoise();
  scan.measurement.glitch = BK4819_GetGlitch();
  scan.measurement.f = scan.currentF;

  scan.scanCycles++;
  UpdateCPS();

  if (scan.mode == SCAN_MODE_ANALYSER) {
    SP_AddPoint(&scan.measurement);
    Reg30_SetPllVco(false);
    scan.currentF += scan.stepF;
    return;
  }

  // Фаза 1: RSSI уже усажен на канал (см. константы выше), но это дешёвый
  // предфильтр — сам по себе не отличает сигнал от шума/наводки
  SquelchPreset sq = GetSqlPresetCached(ctx->squelch.value, scan.currentF);
  bool candidate = scan.measurement.rssi >= sq.ro;
  bool isOpen = false;

  if (candidate) {
    // Фаза 2: досиживаем до ~5мс — тот же критерий, что потом держит
    // LISTENING (HandleStateListening проверяет тот же аппаратный
    // регистр через IsSqOpenGated). Если решать иначе (напр. только по
    // софтовому noise), железо тут же скажет "закрыто" и получим
    // моментальный открыл-закрыл на каждом ложном кандидате.
    SYSTICK_DelayUs(NOISE_CONFIRM_EXTRA_US);
    scan.measurement.noise = BK4819_GetNoise();
    scan.measurement.glitch = BK4819_GetGlitch();
    isOpen = BK4819_IsSquelchOpen();
  }

  SP_AddPoint(&scan.measurement);

  scan.isOpen = isOpen;
  scan.measurement.open = isOpen;
  LOOT_Update(&scan.measurement);

  if (isOpen) {
    vfo->is_open = true;
    RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
    gRedrawScreen = true;

    if (scan.cmdCtx) {
      SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
      if (cmd && (cmd->flags & SCMD_FLAG_AUTO_WHITELIST))
        LOOT_WhitelistLast();
    }

    ChangeState(SCAN_STATE_LISTENING);
  } else {
    Reg30_SetPllVco(false);
    scan.currentF += scan.stepF;
  }
}

static uint32_t sqClosedAt = 0;

static void HandleStateListening(void) {
  if (Now() - scan.radioTimer < SQL_DELAY)
    return;
  scan.radioTimer = Now();

  bool wasOpen = scan.isOpen;
  scan.isOpen = IsSqOpenGated();

  if (scan.isOpen != wasOpen) {
    if (wasOpen) {
      STE_StartGate();
      RADIO_MuteAudioNow(gRadioState);
      sqClosedAt = Now();
    } else {
      vfo->is_open = true;
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
      sqClosedAt = 0;
    }
    gRedrawScreen = true;
  }

  bool shouldLeave;
  if (scan.isOpen) {
    // открыт: уходим по общему таймауту пребывания
    shouldLeave = ElapsedMs() >= SCAN_TIMEOUTS[gSettings.sqOpenedTimeout];
  } else {
    // закрыт: уходим по времени с момента закрытия
    shouldLeave = sqClosedAt && (Now() - sqClosedAt >=
                                 SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
  }

  if (shouldLeave) {
    RADIO_MuteAudioNow(gRadioState);
    scan.currentF += scan.stepF;
    sqClosedAt = 0;
    ChangeState(SCAN_STATE_TUNING);
    gRedrawScreen = true;
  }
}

static void HandleModeSingle(void) {
  scan.measurement.rssi = vfo->msm.rssi;
  scan.measurement.noise = vfo->msm.noise;
  scan.measurement.glitch = vfo->msm.glitch;
  scan.measurement.snr = vfo->msm.snr;

  if (Now() - scan.radioTimer >= SQL_DELAY) {
    RADIO_UpdateSquelch(gRadioState);
    SP_ShiftGraph(-1);
    SP_AddGraphPoint(&scan.measurement);
    scan.radioTimer = Now();
  }
}

// ============================================================================

void SCAN_Check(void) {
  RADIO_CheckAndSaveVFO(gRadioState);
  if (scan.mode == SCAN_MODE_NONE)
    return;

  // мультивотч только в SINGLE — при активном сканировании он конфликтует с
  // радио
  if (scan.mode == SCAN_MODE_SINGLE)
    RADIO_UpdateMultiwatch(gRadioState);

  if (scan.mode == SCAN_MODE_SINGLE) {
    HandleModeSingle();
    return;
  }

  switch (scan.state) {
  case SCAN_STATE_IDLE:
    HandleStateIdle();
    break;
  case SCAN_STATE_TUNING:
    HandleStateTuning();
    break;
  case SCAN_STATE_CHECKING:
    // Больше не используется: проверка шумодава теперь встроена прямо в
    // HandleStateTuning сразу после замера — см. NOISE_SETTLE_EXTRA_US.
    break;
  case SCAN_STATE_LISTENING:
    HandleStateListening();
    break;
  }
}

// ============================================================================

void SCAN_SetMode(ScanMode mode) {
  if (scan.cmdCtx && mode != scan.mode)
    SCAN_SetCommandMode(false);

  scan.mode = mode;
  scan.scanCycles = 0;
  ChangeState(SCAN_STATE_IDLE);

  switch (mode) {
  case SCAN_MODE_SINGLE:
    scan.cmdRangeActive = false;
    scan.currentF = ctx->frequency;
    break;
  case SCAN_MODE_FREQUENCY:
  case SCAN_MODE_ANALYSER:
    ApplyBandSettings();
    BeginScanRange(gCurrentBand.start, gCurrentBand.end,
                   StepFrequencyTable[gCurrentBand.step]);
    break;
  default:
    break;
  }
}

ScanMode SCAN_GetMode(void) { return scan.mode; }

// HandleStateTuning пишет каждую перебираемую частоту прямо в ctx->frequency
// (тот же VFOContext, что и обычный VFO). SCAN_SaveFrequency/RestoreFrequency
// — явная пара для запоминания частоты перед стартом сканирования и её
// восстановления при полном выходе из скана (см. SCANER_init/_deinit).
// Не встроено в SCAN_SetMode(SCAN_MODE_SINGLE): эта функция также
// используется для временной паузы (напр. просмотр Loot List поверх
// активного скана), где восстанавливать частоту не нужно.
void SCAN_SaveFrequency(void) { preScanF = ctx->frequency; }

void SCAN_RestoreFrequency(void) {
  // Кто-то уже явно перестроился (напр. "tune to loot") — не трогаем
  if (ctx->frequency != scan.currentF)
    return;
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, preScanF, false);
  RADIO_ApplySettings(ctx);
}

// Запоминает, где именно в диапазоне остановился свип, чтобы при следующем
// входе в сканер продолжить оттуда, а не с начала диапазона. Вызывать до
// SCAN_RestoreFrequency()/SCAN_SetMode(SCAN_MODE_SINGLE), которые меняют
// scan.currentF.
static uint32_t lastScanF = 0;
void SCAN_SaveScanPosition(void) { lastScanF = scan.currentF; }

// Вызывать после SCAN_SetMode(SCAN_MODE_FREQUENCY)/SCAN_Init — если
// запомненная позиция всё ещё попадает в (возможно новый) диапазон скана,
// продолжаем оттуда вместо scan.startF.
void SCAN_ResumeFromLastPosition(void) {
  if (lastScanF >= scan.startF && lastScanF <= scan.endF)
    scan.currentF = lastScanF;
}

void SCAN_Init(void) {
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;
  scan.radioTimer = Now();
  cachedSqLevel = 0xFF; // форсируем перечитывание пресета на новой сессии скана

  ApplyBandSettings();
  vfo->is_open = false;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
}

void SCAN_SetBand(Band b) {
  gCurrentBand = b;
  UpdateBandAndRestart();
}
void SCAN_SetStartF(uint32_t f) {
  gCurrentBand.start = f;
  UpdateBandAndRestart();
}
void SCAN_SetEndF(uint32_t f) {
  gCurrentBand.end = f;
  UpdateBandAndRestart();
}
void SCAN_SetRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.start = fs;
  gCurrentBand.end = fe;
  UpdateBandAndRestart();
}

void SCAN_Next(void) {
  vfo->is_open = false;
  scan.currentF += scan.stepF;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  ChangeState(SCAN_STATE_TUNING);
}

void SCAN_NextBlacklist(void) {
  LOOT_BlacklistLast();
  gRedrawScreen = true;
  SCAN_Next();
}

void SCAN_NextWhitelist(void) {
  LOOT_WhitelistLast();
  gRedrawScreen = true;
  SCAN_Next();
}

void SCAN_SetDelay(uint32_t delay) { scan.warmupUs = delay; }
uint32_t SCAN_GetDelay(void) { return scan.warmupUs; }
uint32_t SCAN_GetCps(void) { return scan.currentCps; }

// ============================================================================

void SCAN_LoadCommandFile(const char *filename) {
  if (scan.cmdCtx)
    SCAN_SetCommandMode(false);

  scan.cmdCtx = &cmdctx;

  if (SCMD_Init(scan.cmdCtx, filename)) {
    scan.mode = SCAN_MODE_FREQUENCY;
    scan.cmdRangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
  } else {
    scan.cmdCtx = NULL;
    Log("[SCAN] Failed to load: %s", filename);
  }
}

void SCAN_SetCommandMode(bool enabled) {
  if (!enabled && scan.cmdCtx) {
    SCMD_Close(scan.cmdCtx);
    scan.cmdCtx = NULL;
    scan.cmdRangeActive = false;
  }
}

bool SCAN_IsCommandMode(void) { return scan.cmdCtx != NULL; }

void SCAN_CommandForceNext(void) {
  if (!scan.cmdCtx)
    return;
  if (!SCMD_Advance(scan.cmdCtx))
    SCMD_Rewind(scan.cmdCtx);
  scan.cmdRangeActive = false;
  ChangeState(SCAN_STATE_IDLE);
  gRedrawScreen = true;
}

SCMD_Command *SCAN_GetCurrentCommand(void) {
  return scan.cmdCtx ? SCMD_GetCurrent(scan.cmdCtx) : NULL;
}

SCMD_Command *SCAN_GetNextCommand(void) {
  return scan.cmdCtx ? SCMD_GetNext(scan.cmdCtx) : NULL;
}

uint16_t SCAN_GetCommandIndex(void) {
  return scan.cmdCtx ? SCMD_GetCurrentIndex(scan.cmdCtx) : 0;
}

uint16_t SCAN_GetCommandCount(void) {
  return scan.cmdCtx ? SCMD_GetCommandCount(scan.cmdCtx) : 0;
}

// ============================================================================

void SCAN_HandleInterrupt(uint16_t int_bits) {
  const uint16_t tail_mask = BK4819_REG_02_MASK_CxCSS_TAIL |
                             BK4819_REG_02_MASK_CTCSS_LOST |
                             BK4819_REG_02_MASK_CDCSS_LOST;
  if (int_bits & tail_mask) {
    STE_StartGate();
    RADIO_MuteAudioNow(gRadioState);
    scan.isOpen = false;
    gRedrawScreen = true;
  }
}

bool SCAN_IsSqOpen(void) { return BK4819_IsSquelchOpen(); }
const char *SCAN_GetStateName(void) { return SCAN_STATE_NAMES[scan.state]; }
ScanState SCAN_GetState(void) { return scan.state; }
