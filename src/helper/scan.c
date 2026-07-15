#include "scan.h"
#include "../driver/bk4829.h"
#include "../driver/st7565.h"
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

// Адаптивный порог: снижается раз в N шагов без сигнала
#define SQ_DECAY_STEPS 64

// Даже с DIV128 на LCD SPI (см. st7565.c) блит наводит помеху на ближайший
// замер — редких неизбежных редравов (конец свипа, реальный кандидат) не
// избежать, поэтому просто ждём, пока наводка стихнет. Конец свипа дёргает
// UI_ClearScreen() — это все 8 строк экрана грязные разом, полный флеш при
// DIV128 сам по себе занимает ~20-22мс, так что 15мс были короче самого
// блита; берём с запасом
#define RENDER_SETTLE_MS 60

static uint16_t sqLevel = 0;
static uint8_t sqStepsPassed = 0;
static bool sqWasThinking = false;

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

// Детекция сигнала по адаптивному порогу sqLevel
static bool SimpleSq_Check(uint16_t rssi) {
  if (!sqLevel && rssi)
    sqLevel = rssi - 1; // инициализация при первом сигнале
  return rssi >= sqLevel;
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

    // Пропущенные частоты никогда не измеряются, а SP_Begin() не чистит
    // историю графика между свипами — без этого их пиксели навсегда
    // застывают на значении из первого захода в диапазон (обычно 0),
    // образуя периодические "полосы". Продлеваем последний реальный замер
    // до начала пропуска, чтобы дыра каждый свип обновлялась заново.
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

  // precise=false: REG_30 не сбрасывается полностью при перестройке (только
  // импульс ENABLE_VCO_CALIB), DSP не теряет непрерывность между соседними
  // частотами — можно измерять быстрее, без полной паузы на рекалибровку
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);

  SYSTICK_DelayUs(scan.warmupUs);

  // Если экран только что перерисовывался (конец свипа, реальный кандидат),
  // даём наводке от LCD SPI стихнуть перед тем как доверять замеру
  uint32_t sinceBlit = Now() - ST7565_GetLastBlitTime();
  if (sinceBlit < RENDER_SETTLE_MS)
    SYSTICK_DelayMs(RENDER_SETTLE_MS - sinceBlit);

  scan.measurement.rssi = RADIO_GetRSSI(ctx);
  scan.measurement.noise = BK4819_GetNoise();
  scan.measurement.glitch = BK4819_GetGlitch();
  scan.measurement.f = scan.currentF;

  scan.scanCycles++;
  UpdateCPS();

  SP_AddPoint(&scan.measurement);
  if (scan.mode == SCAN_MODE_ANALYSER) {
    Reg30_SetPllVco(false);
    scan.currentF += scan.stepF;
    return;
  }

  if (SimpleSq_Check(scan.measurement.rssi)) {
    ChangeState(SCAN_STATE_CHECKING);
  } else {
    Reg30_SetPllVco(false);
    scan.measurement.open = false;
    scan.currentF += scan.stepF;

    // деградация порога раз в SQ_DECAY_STEPS шагов без сигнала
    if (++sqStepsPassed > SQ_DECAY_STEPS) {
      sqStepsPassed = 0;
      if (!sqWasThinking && sqLevel > 0)
        sqLevel--;
      sqWasThinking = false;
    }
  }
}

#define STE_CONFIRM_MS 60 // окно повторной проверки перед решением

static void HandleStateChecking(void) {
  // Даём каналу устояться STE_CONFIRM_MS без блокировки основного цикла,
  // решение принимаем только после этого окна. Отдельная пауза SQL_DELAY
  // перед этим больше не нужна — устаканивание noise/RSSI теперь сделано
  // в HandleStateTuning (см. SCAN_SPILLOVER_SETTLE_US)
  sqWasThinking = true;
  if (ElapsedMs() < STE_CONFIRM_MS)
    return;

  bool isOpen = BK4819_IsSquelchOpen();
  scan.isOpen = isOpen;
  scan.measurement.open = isOpen;
  scan.measurement.f = scan.currentF;
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
    // ложный кандидат — поднимаем порог, но не выше реального диапазона RSSI
    if (sqLevel < RSSI_MAX)
      sqLevel++;
    scan.currentF += scan.stepF;
    ChangeState(SCAN_STATE_TUNING);
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
    HandleStateChecking();
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

void SCAN_Init(void) {
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;
  scan.radioTimer = Now();
  sqLevel = 0;
  sqStepsPassed = 0;
  sqWasThinking = false;

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

// Активная перестройка/проверка кандидата без прослушивания: аудио ещё не
// звучит (оно включается только в LISTENING), поэтому пропуск опроса
// BK4819 IRQ (DTMF/FSK/STE-tail) здесь безопасен и не заметен пользователю
bool SCAN_IsSweeping(void) {
  return scan.mode == SCAN_MODE_FREQUENCY &&
         (scan.state == SCAN_STATE_TUNING || scan.state == SCAN_STATE_CHECKING);
}

bool SCAN_IsSqOpen(void) { return BK4819_IsSquelchOpen(); }
const char *SCAN_GetStateName(void) { return SCAN_STATE_NAMES[scan.state]; }
ScanState SCAN_GetState(void) { return scan.state; }
