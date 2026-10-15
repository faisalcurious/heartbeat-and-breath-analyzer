// =====================================================================
//  VitalsEngine.cpp  -  see VitalsEngine.h for the pipeline overview
// =====================================================================
#include "VitalsEngine.h"

float DspFFT::s_cos[DSP_FFT_MAX];
bool DspFFT::s_ready = false;

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t s_veMtx = nullptr;
static portMUX_TYPE s_veMux = portMUX_INITIALIZER_UNLOCKED;
#define VE_LOCK()        do { if (s_veMtx) xSemaphoreTake(s_veMtx, portMAX_DELAY); } while (0)
#define VE_UNLOCK()      do { if (s_veMtx) xSemaphoreGive(s_veMtx); } while (0)
#define VE_CRIT_ENTER()  portENTER_CRITICAL(&s_veMux)
#define VE_CRIT_EXIT()   portEXIT_CRITICAL(&s_veMux)
#else
#define VE_LOCK()
#define VE_UNLOCK()
#define VE_CRIT_ENTER()
#define VE_CRIT_EXIT()
#endif

static const float DT = 1.0f / VE_FS;

static inline int16_t clamp16(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32767.0f) return -32767;
  return (int16_t)lrintf(v);
}
static inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Hann window for arbitrary length using the shared cosine table
static inline float hannN(int i, int n) {
  return 0.5f - 0.5f * DspFFT::cosT((int)((float)i * (float)DSP_FFT_MAX / (float)n + 0.5f));
}

// =====================================================================
//  Setup
// =====================================================================
void VitalsEngine::init() {
#if defined(ARDUINO)
  if (!s_veMtx) s_veMtx = xSemaphoreCreateMutex();
#endif
  DspFFT::init();
}

void VitalsEngine::designFilters() {
  const AgeProfile& P = *m_prof;
  for (int i = 0; i < 3; i++) {
    // Cardiac band: SCG / GCG energy lives in ~5-30 Hz, well above breathing,
    // posture drift and the heart-rate fundamental itself.
    m_bpA[i].design(VE_FS, 5.0, 30.0);
    m_bpG[i].design(VE_FS, 5.0, 30.0);
  }
  m_vibHpA.setHighpass(VE_FS, 0.5);
  m_vibHpG.setHighpass(VE_FS, 0.5);
  for (int i = 0; i < 3; i++) {
    m_spdLP[i].setLowpass(VE_FS, 2.5);        // v1.3: chest rotation speed without the heart's own rotation
    m_cfHp[i].setHighpass(VE_FS, 15.0);       // v1.3: cough band 15-45 Hz (explosive air flow, the chest "rings")
    m_cfLp[i].setLowpass(VE_FS, 45.0);
    m_lfLp[i].setLowpass(VE_FS, 8.0);         // v1.3: body-movement band 0.5-8 Hz
  }

  const double fs10 = (double)VE_FS / VE_RESP_DIV;
  double fHi = dspClamp(P.rrSearchHi / 60.0f * 1.25f, 0.6f, 2.0f);       // upper breathing band edge
  double fAA = dspClamp((float)(fHi * 1.5), 2.0f, 2.8f);                  // anti-alias before 10 Hz
  for (int i = 0; i < 3; i++) {
    m_respAA[i].setLowpass(VE_FS, fAA);
    m_respHP[i].setHighpass(fs10, 0.04);      // removes posture drift, keeps >= 3 br/min
    m_respLP[i].setLowpass(fs10, fHi);
  }
}

void VitalsEngine::begin(const CalibData& cal, float ageYears) {
  m_running = false;
  m_cal = cal;
  m_ageYears = ageYears; m_pendingAge = ageYears; m_agePending = false; m_flipRequest = false;
  m_prof = &ageProfileFor(ageYears);
  VE_CRIT_ENTER();
  m_n = 0; m_baseMs = 0; m_baseN = 0;
  VE_CRIT_EXIT();
  m_gapSamples = 0;

  for (int i = 0; i < 3; i++) { m_g[i] = cal.g0[i]; m_rotI[i] = 0; m_bias[i] = cal.gyroBias[i]; }
  m_turning = false;
  m_motG = m_motA = 0;
  // Initial accelerometer offset: the part along gravity is observable from
  // |g| at rest (clone MPU6050s often read 1.1-1.3 g). It is refined later.
  {
    float e = (cal.gMag > 0.5f && cal.gMag < 1.6f) ? cal.gMag - 1.0f : 0.0f;
    for (int i = 0; i < 3; i++) { m_accB[i] = e * cal.g0[i]; m_hpA[i] = 0; m_hpPrev[i] = cal.g0[i]; m_theta[i] = 0; }
  }
  m_magLP = 1.0f;
  m_motGh = 0;
  for (int i = 0; i < 3; i++) { m_hpL[i] = 0; m_hpLprev[i] = 0; m_hpW[i] = 0; m_hpWprev[i] = 0; }
  m_cfAccH = m_cfAccL = m_cfAccW = 0; m_cfAccN = 0; m_cfHead = 0; m_cfDone = 0; m_cfBase = 0; m_coughTot = 0;
  memset(m_cfH, 0, sizeof(m_cfH)); memset(m_cfL, 0, sizeof(m_cfL)); memset(m_cfW, 0, sizeof(m_cfW)); memset(m_spd, 0, sizeof(m_spd));
  m_brPerEma = 60.0f / dspClamp(0.5f * (ageProfileFor(ageYears).rrLo + ageProfileFor(ageYears).rrHi), 6.0f, 60.0f); m_brTotal = 0;
  m_artEma = 0; m_lastGoodHr = NAN; m_subHarmN = 0;
  m_rmsS = m_rmsL = 0; m_fastAdaptUntil = 0;
  m_motionUntil = 2 * VE_FS;  // first 2 s = filter settling, treated like motion
  m_motion = m_prevMotion = true; m_startupMotion = true; m_motionStartN = 0;
  m_postureN = 0; m_postureFlag = false; m_tilt = 0;

  designFilters();
  for (int i = 0; i < 3; i++) { m_bpA[i].reset(); m_bpG[i].reset(); m_respAA[i].reset(); m_respHP[i].reset(); m_respLP[i].reset();
                                m_spdLP[i].reset(); m_cfHp[i].reset(); m_cfLp[i].reset(); m_lfLp[i].reset(); }
  m_vibHpA.reset(); m_vibHpG.reset();
  memset(m_covA, 0, sizeof(m_covA)); memset(m_covG, 0, sizeof(m_covG)); memset(m_covR, 0, sizeof(m_covR));
  memset(m_dlA, 0, sizeof(m_dlA)); memset(m_dlG, 0, sizeof(m_dlG)); memset(m_dlR, 0, sizeof(m_dlR));
  m_lastMotionN = 0; m_lastMotionN10 = 0;
  m_vA[0] = 0; m_vA[1] = 0; m_vA[2] = 1;
  m_vG[0] = 0; m_vG[1] = 0; m_vG[2] = 1;
  // respiration axis seed: any vector perpendicular to gravity
  {
    float ref[3] = {1, 0, 0};
    if (fabsf(cal.g0[0]) > 0.8f) { ref[0] = 0; ref[1] = 1; }
    float c[3] = {cal.g0[1] * ref[2] - cal.g0[2] * ref[1], cal.g0[2] * ref[0] - cal.g0[0] * ref[2], cal.g0[0] * ref[1] - cal.g0[1] * ref[0]};
    float nn = sqrtf(dot3(c, c));
    if (nn > 1e-6f) { m_vR[0] = c[0] / nn; m_vR[1] = c[1] / nn; m_vR[2] = c[2] / nn; }
  }
  m_normA = 4.0f; m_normG = 0.01f;
  m_maA.reset(); m_maG.reset();
  m_wA = 0.5f; m_wG = 0.5f;
  m_envAcc = 0; m_envAccN = 0;
  memset(m_envA, 0, sizeof(m_envA)); memset(m_envG, 0, sizeof(m_envG)); memset(m_envMotion, 1, sizeof(m_envMotion));
  memset(m_envS, 0, sizeof(m_envS)); memset(m_vibA, 0, sizeof(m_vibA)); memset(m_vibG, 0, sizeof(m_vibG));
  m_envHead = 0; m_envSHead = 0; m_vibHead = 0;
  m_coughN = 0; m_coughLastIdx = 0; m_coughBase = 0; m_vRmsA = m_vRmsG = m_vPkA = m_vHf = NAN;

  memset(m_dHist, 0, sizeof(m_dHist));
  m_spk = 2.5f; m_npk = 1.0f;
  m_haveBeat = m_chainOk = m_cand = false;
  m_lastBeatN = m_candN = m_bestRejN = 0; m_lastBeatT = 0; m_candVal = 0; m_bestRejVal = 0; m_lastOff = 0;
  m_expRR = 0.8f * VE_FS; m_ibiMedN = 0; m_ibiMedIdx = 0; m_rejN = 0;
  m_learnUntil = 0; m_learnMax = 0; m_learnSum = 0; m_learnCnt = 0; m_noBeatN = 0;

  m_polarity = 1; m_autoPolarity = true; m_polScore = 0; m_polCount = 0;
  m_n10 = 0; memset(m_respMotion, 0xFF, sizeof(m_respMotion)); m_primeResp = true;
  resetBreathDetector(0);
  m_ampEma = 0.3f; m_brCount = 0; m_inPause = false; m_lastConfN = 0; m_lastExtN = 0;

  m_prevLag = 0; m_presence = 0; m_acfHistN = 0; m_acfHistIdx = 0; m_analyzeCount = 0;
  m_lastRR = NAN; m_lastHR = NAN;

  disp.reset(); resp.reset(); beats.reset(); breaths.reset(); trend.reset(); events.reset();
  respPeakN10 = 0;

  VE_LOCK();
  m_res = VitalsResults();
  m_spec = VitalsSpectra();
  VE_UNLOCK();

  addEvent(EV_RECAL, 0, 0);
  m_session++;
  m_running = true;
}

void VitalsEngine::setFsActual(float fs) {
  if (fs < 150.0f || fs > 250.0f) return;
  VE_CRIT_ENTER();
  double nowMs = m_baseMs + (double)((int32_t)(m_n - m_baseN)) * 1000.0 / m_fs;
  m_baseMs = nowMs; m_baseN = m_n;
  m_fs = 0.7f * m_fs + 0.3f * fs;
  VE_CRIT_EXIT();
}

uint32_t VitalsEngine::msOfSample(uint32_t n) const {
  VE_CRIT_ENTER();
  double t = m_baseMs + (double)((int32_t)(n - m_baseN)) * 1000.0 / m_fs;
  VE_CRIT_EXIT();
  return t < 0 ? 0 : (uint32_t)t;
}

void VitalsEngine::addEvent(uint8_t type, uint32_t tMs, uint32_t durMs) {
  VEvent e; memset(&e, 0, sizeof(e));
  e.tMs = tMs; e.durMs = durMs; e.type = type;
  events.push(e);
}

void VitalsEngine::applyPending() {
  if (m_agePending) {
    m_agePending = false;
    m_ageYears = m_pendingAge;
    m_prof = &ageProfileFor(m_ageYears);
    designFilters();
    m_primeResp = true;
  }
  if (m_flipRequest) {
    m_flipRequest = false;
    m_polarity = -m_polarity;
    m_autoPolarity = false;
    resetBreathDetector(0);
  }
}

void VitalsEngine::resetBreathDetector(float y) {
  m_brState = 0; m_brExt = y; m_brExtN = m_n10;
  m_haveTrough = m_havePeak = false; m_brMotionSeen = false;
  m_troughN = m_peakN = 0; m_troughV = m_peakV = 0;
}

void VitalsEngine::pushGap(int missing) {
  if (!m_running || missing <= 0) return;
  if (missing > 2 * VE_FS) missing = 2 * VE_FS;
  float ax = m_lastRaw[0], ay = m_lastRaw[1], az = m_lastRaw[2];
  for (int i = 0; i < missing; i++) pushSample(ax, ay, az, m_bias[0], m_bias[1], m_bias[2]);  // hold attitude, zero rotation
  m_gapSamples += missing;
  m_chainOk = false;           // the interval across a gap is unknown
  m_brMotionSeen = true;
  m_cand = false;
}

// =====================================================================
//  Per-sample processing (200 Hz, sensor task)
// =====================================================================
void VitalsEngine::pushSample(float ax, float ay, float az, float gx, float gy, float gz) {
  m_lastRaw[0] = ax; m_lastRaw[1] = ay; m_lastRaw[2] = az; m_lastRaw[3] = gx; m_lastRaw[4] = gy; m_lastRaw[5] = gz;
  if (!m_running) return;
  applyPending();
  const uint32_t n = m_n;

  // ---- calibrated physical values ----------------------------------
  float a[3] = {ax * m_cal.accScale - m_accB[0], ay * m_cal.accScale - m_accB[1], az * m_cal.accScale - m_accB[2]};
  float wd[3] = {gx - m_bias[0], gy - m_bias[1], gz - m_bias[2]};   // dps
  float amag = sqrtf(dot3(a, a));
  float wmag2 = dot3(wd, wd);
  m_magLP += (DT / 2.0f) * (amag - m_magLP);

  // ---- attitude: gravity vector in the sensor frame ---------------
  // A world-fixed vector seen from a rotating body obeys dg/dt = -w x g.
  // Gyro propagation keeps the fast (breathing) tilt accurate; a slow
  // accelerometer correction (tau 1.5 s) removes gyro drift. The accel is
  // used only when free of dynamic acceleration, judged against its own
  // recent magnitude (not against 1 g), so a sensor offset cannot block it.
  {
    float w0 = wd[0] * DSP_DEG2RAD, w1 = wd[1] * DSP_DEG2RAD, w2 = wd[2] * DSP_DEG2RAD;
    float c0 = w1 * m_g[2] - w2 * m_g[1];
    float c1 = w2 * m_g[0] - w0 * m_g[2];
    float c2 = w0 * m_g[1] - w1 * m_g[0];
    m_g[0] -= DT * c0; m_g[1] -= DT * c1; m_g[2] -= DT * c2;
    if (amag > 0.5f && amag < 1.6f && fabsf(amag - m_magLP) < 0.06f) {
      float kA = (n < VE_FS) ? DT / 0.2f : DT / 1.5f;
      float inv = 1.0f / amag;
      for (int i = 0; i < 3; i++) m_g[i] += kA * (a[i] * inv - m_g[i]);
    }
    float gn = sqrtf(dot3(m_g, m_g));
    if (gn > 1e-6f) { m_g[0] /= gn; m_g[1] /= gn; m_g[2] /= gn; }
  }
  float lin[3] = {a[0] - m_g[0], a[1] - m_g[1], a[2] - m_g[2]};  // g

  // ---- motion / artefact detection --------------------------------
  // v1.3: breathing itself must never look like movement. Deep or fast
  // breaths tilt the chest by several degrees, which changes the gravity
  // seen by the accelerometer by 0.1 g or more - v1.2 high-passed the RAW
  // acceleration and flagged deep breaths as movement (flat breathing
  // trace). Now the accelerometer criterion uses the LINEAR acceleration
  // (gravity removed with the gyro-tracked attitude, so tilt is excluded)
  // and the gyro criterion uses the rotation FASTER than breathing (1.5 Hz
  // high-pass), plus a high limit on the total rotation for slow turns.
  {
    const float kM = DT / 0.3f;
    for (int i = 0; i < 3; i++) {
      m_hpA[i] = 0.9845f * (m_hpA[i] + a[i] - m_hpPrev[i]); m_hpPrev[i] = a[i];           // raw (kept for reference)
      m_hpL[i] = 0.9845f * (m_hpL[i] + lin[i] - m_hpLprev[i]); m_hpLprev[i] = lin[i];      // linear, 0.5 Hz HP
      m_hpW[i] = 0.9550f * (m_hpW[i] + wd[i] - m_hpWprev[i]); m_hpWprev[i] = wd[i];        // rotation, 1.5 Hz HP
    }
    m_motG += kM * (wmag2 - m_motG);
    m_motGh += kM * (dot3(m_hpW, m_hpW) - m_motGh);
    m_motA += kM * (dot3(m_hpL, m_hpL) - m_motA);
    // Large slow rotation about ANY axis = change of posture (turning over, sitting up, twisting). Even a very
    // deep breath rotates the chest only a few degrees either side of its mean position, so a departure of more
    // than 18 degrees from the slowly following mean orientation (leaky rotation integral, tau 8 s) is movement.
    bool turn = false;
    {
      const float leak = 1.0f - DT / 8.0f;
      for (int i = 0; i < 3; i++) m_rotI[i] = m_rotI[i] * leak + wd[i] * DT;
      if (dot3(m_rotI, m_rotI) > 18.0f * 18.0f) { turn = true; m_turning = true; m_rotI[0] = m_rotI[1] = m_rotI[2] = 0; }
      // once a turn is detected it lasts as long as the chest keeps rotating (the tail of the turn is movement too)
      if (m_turning) { if (m_motG > 5.0f * 5.0f) turn = true; else m_turning = false; }
    }
    bool raw = turn || (m_motGh > 10.0f * 10.0f) || (m_motG > 30.0f * 30.0f) || (m_motA > 0.04f * 0.04f) || (fabsf(amag - m_magLP) > 0.3f);
    if (raw) {
      m_motionUntil = n + 2 * VE_FS;   // hold 2 s after the last artefact
      if (!m_prevMotion) m_startupMotion = false;
    }
  }
  const bool motion = (n < m_motionUntil);
  if (motion && !m_prevMotion) { m_motionStartN = n; m_startupMotion = false; }
  if (!motion && m_prevMotion) {
    if (!m_startupMotion) addEvent(EV_MOTION, msOfSample(m_motionStartN), (uint32_t)((n - m_motionStartN) * 1000.0f / m_fs));
    m_startupMotion = false;
    // restart the breathing channel from the new position
    for (int i = 0; i < 3; i++) { m_theta[i] = 0; m_respAA[i].prime(0.0f); }
    m_primeResp = true;
    m_learnUntil = n + 3 * VE_FS; m_learnMax = 0; m_learnSum = 0; m_learnCnt = 0;
    m_noBeatN = n;
  }
  m_motion = motion; m_prevMotion = motion;

  // ---- accelerometer offset tracking ------------------------------
  // At rest |a| must be exactly 1 g. The error is fed back along the
  // measured gravity direction (the observable offset component), which
  // follows temperature drift and adapts after posture changes (tau 30 s).
  if (!motion && m_motG < 4.0f * 4.0f && amag > 0.5f && amag < 1.6f) {
    const float kb = (n < 20 * VE_FS) ? DT / 5.0f : DT / 30.0f;
    const float e = kb * (amag - 1.0f) / amag;
    for (int i = 0; i < 3; i++) m_accB[i] = dspClamp(m_accB[i] + e * a[i], -0.6f, 0.6f);
  }

  // ---- slow gyro-bias tracking while still ------------------------
  if (!motion && wmag2 < 25.0f) {
    const float kb = DT / 60.0f;
    m_bias[0] += kb * (gx - m_bias[0]);
    m_bias[1] += kb * (gy - m_bias[1]);
    m_bias[2] += kb * (gz - m_bias[2]);
  }

  // =================================================================
  //  HEART: SCG (linear acceleration) + GCG (angular rate)
  // =================================================================
  if (n == 0) {
    for (int i = 0; i < 3; i++) { m_bpA[i].prime(lin[i] * 1000.0f); m_bpG[i].prime(wd[i]); }
  }
  float xa[3], xg[3];
  for (int i = 0; i < 3; i++) {
    xa[i] = m_bpA[i].process(lin[i] * 1000.0f);   // mg
    xg[i] = m_bpG[i].process(wd[i]);              // dps
  }
  const bool warm = n < 2 * VE_FS;
  // Adaptive learning (PCA axes, energy scale) uses samples delayed by 0.6 s
  // and only when no motion was flagged since, so the first fraction of a
  // second of a movement (before the motion detector latches) is never learnt.
  // v1.3: the adaptive heart axes and scales also stop learning at the START of any rotation faster than
  // breathing (8 deg/s, or 4 deg/s above 1.5 Hz), before the movement detector has decided - so the onset of a
  // turn can never teach them the wrong axis. (Breathing alone stays well below these limits.)
  if (motion || m_motG > 8.0f * 8.0f || m_motGh > 4.0f * 4.0f) m_lastMotionN = n;
  const int di = (int)(n % VE_GATE_DELAY);
  float da[3] = {m_dlA[di][0], m_dlA[di][1], m_dlA[di][2]};
  float dg[3] = {m_dlG[di][0], m_dlG[di][1], m_dlG[di][2]};
  for (int i = 0; i < 3; i++) { m_dlA[di][i] = xa[i]; m_dlG[di][i] = xg[i]; }
  const bool learnOk = n >= VE_GATE_DELAY && (n - m_lastMotionN) > VE_GATE_DELAY;
  if (learnOk) {
    dspCovUpdateRobust(m_covA, da, DT / 6.0f, 25.0f);
    dspCovUpdateRobust(m_covG, dg, DT / 6.0f, 25.0f);
  }
  if ((n % 100) == 0) {                     // dominant cardiac axes (adaptive PCA)
    dspPowerIter3(m_covA, m_vA, 3);
    dspPowerIter3(m_covG, m_vG, 3);
  }
  const float sa = dot3(m_vA, xa);          // SCG on dominant axis (mg)
  const float sg = dot3(m_vG, xg);          // GCG on dominant axis (dps)

  // raw (un-band-passed) projections for the 0-50 Hz vibration spectrum
  {
    float ra = m_vibHpA.process(dot3(m_vA, lin) * 1000.0f);
    float rg = m_vibHpG.process(dot3(m_vG, wd));
    uint32_t vi = m_vibHead % VE_VIB_LEN;
    m_vibA[vi] = clamp16(ra * 100.0f); m_vibG[vi] = clamp16(rg * 1000.0f);
    m_vibHead = m_vibHead + 1;
  }

  // normalised energies (each channel has mean ~1 when still)
  const float ea = sa * sa, eg = sg * sg;
  // Robust normalisers: energy is clipped at 6x the running mean before it
  // enters the average, so a movement burst (seen before the motion detector
  // latches) cannot inflate the scale and blind the detector afterwards.
  if (learnOk) {
    const float eda = dspSq(dot3(m_vA, da)), edg = dspSq(dot3(m_vG, dg));
    m_normA += (DT / 6.0f) * (fminf(eda, 6.0f * m_normA) - m_normA);
    m_normG += (DT / 6.0f) * (fminf(edg, 6.0f * m_normG) - m_normG);
  } else if (warm && n > VE_FS / 2) {
    m_normA += (DT / 0.5f) * (ea - m_normA);
    m_normG += (DT / 0.5f) * (eg - m_normG);
  }
  float na = motion ? 0.0f : dspClamp(ea / (m_normA + 1e-9f), 0.0f, 50.0f);
  float ng = motion ? 0.0f : dspClamp(eg / (m_normG + 1e-9f), 0.0f, 50.0f);
  const float envA = m_maA.push(na);        // 80 ms moving average = energy envelope
  const float envG = m_maG.push(ng);
  const float E = m_wA * envA + m_wG * envG;  // quality-weighted fusion
  const float D = sqrtf(E > 0 ? E : 0);
  beatStep(D, motion);

  // v1.3: chest rotation speed (breathing movement) next to the heart envelopes, for the breathing-artefact check
  float wl[3];
  for (int i = 0; i < 3; i++) wl[i] = m_spdLP[i].process(wd[i]);
  if ((n % VE_ENV_DIV) == 0) {
    uint32_t ei = m_envHead % VE_ENV_LEN;
    m_envA[ei] = sqrtf(envA > 0 ? envA : 0);
    m_envG[ei] = sqrtf(envG > 0 ? envG : 0);
    m_spd[ei] = sqrtf(dot3(wl, wl));
    m_envMotion[ei] = motion ? 1 : 0;
    m_envHead = m_envHead + 1;
  }
  // v1.3: 50 ms frames for the cough detector: high band (15-45 Hz), body-movement band (0.5-8 Hz), fast rotation
  {
    float eh = 0, el = 0;
    for (int i = 0; i < 3; i++) {
      float h = m_cfLp[i].process(m_cfHp[i].process(lin[i] * 1000.0f));
      float l = m_lfLp[i].process(m_hpL[i] * 1000.0f);
      eh += h * h; el += l * l;
    }
    m_cfAccH += eh; m_cfAccL += el; m_cfAccW += dot3(m_hpW, m_hpW); m_cfAccN++;
    if (m_cfAccN >= 10) {
      const uint32_t fi = m_cfHead & 127;
      m_cfH[fi] = sqrtf(m_cfAccH / m_cfAccN); m_cfL[fi] = sqrtf(m_cfAccL / m_cfAccN); m_cfW[fi] = sqrtf(m_cfAccW / m_cfAccN);
      m_cfHead = m_cfHead + 1;
      m_cfAccH = m_cfAccL = m_cfAccW = 0; m_cfAccN = 0;
    }
  }
  m_envAcc += D; m_envAccN++;
  if (m_envAccN >= VE_ENVS_DIV) {
    m_envS[m_envSHead % VE_ENVS_LEN] = m_envAcc / m_envAccN;
    m_envSHead = m_envSHead + 1;
    m_envAcc = 0; m_envAccN = 0;
  }
  if ((n % VE_DISP_DIV) == 0) {
    DispSample ds;
    ds.scg = clamp16(sa * 100.0f);
    ds.gcg = clamp16(sg * 1000.0f);
    ds.env = clamp16(D * 100.0f);
    disp.push(ds);
  }

  // =================================================================
  //  RESPIRATION: chest-wall tilt from the gravity vector
  // =================================================================
  // Breathing rotates the chest wall. Integrating the angular rate gives
  // that rotation about ALL three axes (the gravity direction only sees
  // rotation about horizontal axes) and is immune to accelerometer offsets.
  for (int i = 0; i < 3; i++) m_theta[i] += wd[i] * DT;              // degrees
  if (fabsf(m_theta[0]) + fabsf(m_theta[1]) + fabsf(m_theta[2]) > 3000.0f) {   // keep float precision
    for (int i = 0; i < 3; i++) { m_theta[i] = 0; m_respAA[i].prime(0.0f); }
    m_primeResp = true;
  }
  if (n == 0) for (int i = 0; i < 3; i++) m_respAA[i].prime(m_theta[i]);
  float rl[3];
  for (int i = 0; i < 3; i++) rl[i] = m_respAA[i].process(m_theta[i]);
  if ((n % VE_RESP_DIV) == 0) respStep(rl, motion);

  m_n = n + 1;
}

// ---------------------------------------------------------------------
//  Respiration at 10 Hz
// ---------------------------------------------------------------------
void VitalsEngine::respStep(const float rl[3], bool motion) {
  const uint32_t n10 = m_n10;
  const float fs10 = m_fs / VE_RESP_DIV;
  if (m_primeResp) {
    for (int i = 0; i < 3; i++) { m_respHP[i].prime(rl[i]); m_respLP[i].reset(); }
    m_primeResp = false;
    resetBreathDetector(0);
    m_rmsS = 0;                                   // re-learn the level in the new position
    m_fastAdaptUntil = n10 + (uint32_t)(15 * fs10);
  }
  float r[3];
  for (int i = 0; i < 3; i++) r[i] = m_respLP[i].process(m_respHP[i].process(rl[i]));
  // same delayed, motion-gated learning for the breathing axis (tau 40 s)
  if (motion) m_lastMotionN10 = n10;
  {
    const int j = (int)(n10 & 7);
    float d[3] = {m_dlR[j][0], m_dlR[j][1], m_dlR[j][2]};
    for (int i = 0; i < 3; i++) m_dlR[j][i] = r[i];
    if (n10 >= 8 && (n10 - m_lastMotionN10) > 8) {
      if (n10 < 300) dspCovUpdate(m_covR, d, 1.0f / (10.0f * 40.0f));
      else dspCovUpdateRobust(m_covR, d, 1.0f / (10.0f * 40.0f), 16.0f);
    }
  }
  if ((n10 % 10) == 0) dspPowerIter3(m_covR, m_vR, 4);
  // 1-D breathing signal: deviation of gravity direction along the principal
  // breathing axis. Unit vector change in rad -> degrees of chest-wall tilt.
  float y = (float)m_polarity * dot3(m_vR, r);                        // degrees of rotation

  resp.push(clamp16(y * 1000.0f));
  uint32_t bi = n10 % VE_RESP_LEN;
  if (motion) m_respMotion[bi >> 3] |= (uint8_t)(1u << (bi & 7));
  else m_respMotion[bi >> 3] &= (uint8_t)~(1u << (bi & 7));

  if ((n10 % 10) == 0) {                     // posture vs calibration, 1 Hz
    float d = dspClamp(dot3(m_g, m_cal.g0), -1.0f, 1.0f);
    m_tilt = acosf(d) * DSP_RAD2DEG;
    if (m_tilt > 30.0f) {
      if (!m_postureFlag && ++m_postureN >= 5) { m_postureFlag = true; addEvent(EV_POSTURE, msOfSample(m_n), 0); }
    } else if (m_tilt < 20.0f) { m_postureN = 0; m_postureFlag = false; }
  }

  breathDetect(y, n10, motion);
  m_n10 = n10 + 1;
  (void)fs10;
}

void VitalsEngine::breathDetect(float y, uint32_t n10, bool motion) {
  const float fs10 = m_fs / VE_RESP_DIV;
  const AgeProfile& P = *m_prof;

  if (motion) {
    m_brMotionSeen = true;
    if (m_inPause) {                          // pause interrupted by movement
      uint32_t durMs = (uint32_t)((n10 - m_lastConfN) * 1000.0f / fs10);
      addEvent(EV_PAUSE, msOfSample(m_lastConfN * VE_RESP_DIV), durMs);
      m_inPause = false;
    }
    m_lastConfN = n10;
  }
  if (!m_inPause && !motion && (n10 - m_lastConfN) >= (uint32_t)(P.apneaSec * fs10) && m_brCount >= 3) m_inPause = true;

  // Adaptive levels from the signal itself (independent of detection, so
  // they follow the breathing both up AND down, e.g. sitting -> lying):
  // short-term power (tau 8 s, 2 s while re-learning after movement) sets the
  // peak/trough hysteresis; long-term power (tau 60 s) sets the minimum
  // depth of a valid breath. 2.6 x RMS ~ peak-to-peak of a breathing wave.
  if (!motion) {
    // v1.3: the short-term level follows the breathing within ~4 s (8 s in v1.2) and decays faster when no
    // turning point has been confirmed for longer than about one breath. After a few deep breaths the
    // hysteresis therefore shrinks back within seconds and the following normal breaths are found again
    // (v1.2 could miss them for 10 s or more and report a false breathing pause).
    const bool stalled = (float)(n10 - m_lastConfN) > fs10 * fmaxf(2.5f, 0.9f * m_brPerEma);
    const float tauS = (n10 < m_fastAdaptUntil) ? 2.0f : stalled ? 1.2f : 4.0f;
    m_rmsS += (1.0f / (fs10 * tauS)) * (y * y - m_rmsS);
    m_rmsL += (1.0f / (fs10 * 60.0f)) * (y * y - m_rmsL);
  }
  const float ppS = 2.6f * sqrtf(m_rmsS), ppL = 2.6f * sqrtf(m_rmsL);
  const float h = fmaxf(0.3f * ppS, 0.05f);        // hysteresis (deg); floor well above gyro noise
  if (m_brState == 1) {                            // rising: looking for end-inspiration peak
    if (y > m_brExt) { m_brExt = y; m_brExtN = n10; }
    else if (y < m_brExt - h) {
      m_peakN = m_brExtN; m_peakV = m_brExt; m_havePeak = true; respPeakN10 = m_peakN;
      m_brState = 0; m_brExt = y; m_brExtN = n10;
      m_lastExtN = m_peakN;
      if (m_inPause) { addEvent(EV_PAUSE, msOfSample(m_lastConfN * VE_RESP_DIV), (uint32_t)((n10 - m_lastConfN) * 1000.0f / fs10)); m_inPause = false; }
      m_lastConfN = n10;
    }
    return;
  }
  // falling: looking for end-expiration trough
  if (y < m_brExt) { m_brExt = y; m_brExtN = n10; return; }
  if (y <= m_brExt + h) return;

  const uint32_t tn = m_brExtN; const float tv = m_brExt;
  m_brState = 1; m_brExt = y; m_brExtN = n10; m_lastExtN = tn;
  if (m_inPause) { addEvent(EV_PAUSE, msOfSample(m_lastConfN * VE_RESP_DIV), (uint32_t)((n10 - m_lastConfN) * 1000.0f / fs10)); m_inPause = false; }
  m_lastConfN = n10;

  if (m_haveTrough && m_havePeak && m_peakN > m_troughN && tn > m_peakN) {
    float per = (tn - m_troughN) / fs10;
    float ti = (m_peakN - m_troughN) / fs10;
    float te = (tn - m_peakN) / fs10;
    float amp = m_peakV - 0.5f * (m_troughV + tv);
    bool rangeOk = per >= 0.85f * 60.0f / P.rrSearchHi && per <= 60.0f / P.rrSearchLo;
    // v1.3: minimum depth relative to the CURRENT breathing level as well as the 1-minute level, so normal or
    // fast shallow breaths right after deep ones are still counted (v1.2 used the 1-minute level only)
    bool ampOk = amp >= fmaxf(0.06f, fminf(0.2f * ppL, 0.4f * ppS));
    bool valid = rangeOk && ampOk && !m_brMotionSeen;
    uint8_t fl = valid ? BR_VALID : 0;
    if (m_brMotionSeen) fl |= BR_MOTION;
    if (valid && m_brCount >= 5 && amp > 2.2f * m_ampEma) fl |= BR_SIGH;
    if (valid) {
      float k = m_brCount < 5 ? 0.3f : 0.1f;
      m_ampEma += k * (fminf(amp, 3.0f * m_ampEma) - m_ampEma);
      if (m_ampEma < 0.02f) m_ampEma = 0.02f;
      m_brCount++;
      m_brTotal++;
      m_brPerEma += 0.25f * (dspClamp(per, 0.6f, 20.0f) - m_brPerEma);
    }
    BreathEvent be; memset(&be, 0, sizeof(be));
    be.n10 = tn;
    be.periodMs = (uint16_t)dspClamp(per * 1000.0f, 0, 65535);
    be.tiMs = (uint16_t)dspClamp(ti * 1000.0f, 0, 65535);
    be.teMs = (uint16_t)dspClamp(te * 1000.0f, 0, 65535);
    be.ampMdeg = clamp16(amp * 1000.0f);
    be.flags = fl;
    breaths.push(be);

    // Polarity learning: in normal breathing inspiration is shorter than
    // expiration (Ti/Ttot ~ 0.4). If the opposite is consistently seen the
    // PCA sign is upside down, so flip it (only while in automatic mode).
    if (valid && m_autoPolarity && ti > 0 && te > 0) {
      float s = dspClamp((te - ti) / (te + ti), -1.0f, 1.0f);
      m_polScore = 0.9f * m_polScore + 0.1f * s;
      if (++m_polCount >= 10 && m_polScore < -0.12f) {
        m_polarity = -m_polarity;
        m_polScore = 0; m_polCount = 0;
        resetBreathDetector(-y);
        m_brExt = 1e9f;
        return;
      }
    }
  }
  m_troughN = tn; m_troughV = tv; m_haveTrough = true; m_brMotionSeen = false;
}

// ---------------------------------------------------------------------
//  Beat detector on the fused envelope (200 Hz)
//  Pan-Tompkins style signal/noise peak levels, refractory period tied to
//  the expected interval, confirmation delay and search-back.
// ---------------------------------------------------------------------
void VitalsEngine::beatStep(float D, bool motion) {
  const uint32_t n = m_n;
  m_dHist[n & 63] = D;
  if (motion) {
    m_cand = false; m_bestRejVal = 0; m_chainOk = false; m_haveBeat = false;
    return;
  }
  const AgeProfile& P = *m_prof;
  if (m_reseedRR > 0) {                           // v1.3: the analysis found the true rhythm: restart the interval model
    m_expRR = m_reseedRR; m_reseedRR = 0;
    m_ibiMedN = 0; m_ibiMedIdx = 0; m_rejN = 0; m_chainOk = false;
  }
  // learning window after each still period starts: set levels from data
  if (m_learnUntil) {
    if (n < m_learnUntil) {
      if (D > m_learnMax) m_learnMax = D;
      m_learnSum += D; m_learnCnt++;
    } else {
      if (m_learnCnt > 0) {
        m_npk = fmaxf(0.3f, m_learnSum / m_learnCnt);
        m_spk = fmaxf(m_npk * 1.6f, 0.6f * m_learnMax);
      }
      m_learnUntil = 0;
    }
  }
  const float refr = dspClamp(0.62f * m_expRR, 0.8f * 60.0f / P.hrSearchHi * VE_FS, 1.2f * VE_FS);

  if (n >= 2) {                                   // local maximum at n-1
    float y0 = m_dHist[(n - 2) & 63], y1 = m_dHist[(n - 1) & 63];
    if (y1 > y0 && y1 >= D) {
      uint32_t pn = n - 1;
      bool inRefr = m_haveBeat && (float)(pn - m_lastBeatN) < refr;
      if (!inRefr && (!m_cand || y1 > m_candVal)) { m_cand = true; m_candVal = y1; m_candN = pn; }
    }
  }
  const float thr = m_npk + 0.30f * (m_spk - m_npk);
  if (m_cand && (n - m_candN) >= (uint32_t)(0.12f * VE_FS)) {
    if (m_candVal >= thr) {
      acceptBeat(m_candN, m_candVal, 0);
      m_spk = 0.875f * m_spk + 0.125f * m_candVal;
      m_bestRejVal = 0;
    } else {
      m_npk = 0.875f * m_npk + 0.125f * m_candVal;
      if (m_candVal > m_bestRejVal) { m_bestRejVal = m_candVal; m_bestRejN = m_candN; }
    }
    m_cand = false;
  }
  // search-back for a missed beat
  if (m_haveBeat && (float)(n - m_lastBeatN) > 1.66f * m_expRR && m_bestRejVal > 0.5f * thr &&
      (float)(m_bestRejN - m_lastBeatN) > refr && m_bestRejN > m_lastBeatN) {
    acceptBeat(m_bestRejN, m_bestRejVal, BEAT_SEARCHBACK);
    m_spk = 0.75f * m_spk + 0.25f * m_bestRejVal;
    m_bestRejVal = 0;
  }
  // no beats for a long time: let the signal level decay towards noise
  uint32_t since = m_haveBeat ? n - m_lastBeatN : n - m_noBeatN;
  if ((float)since > 2.0f * m_expRR && m_spk > 1.6f * m_npk) m_spk *= 0.998f;
}

void VitalsEngine::acceptBeat(uint32_t bn, float val, uint8_t extraFlags) {
  const AgeProfile& P = *m_prof;
  float off = 0;
  if (m_n - bn < 60 && bn >= 1) off = dspParabolic(m_dHist[(bn - 1) & 63], m_dHist[bn & 63], m_dHist[(bn + 1) & 63]);
  uint8_t fl = extraFlags;
  uint16_t ibiMs = 0;
  if (m_haveBeat && m_chainOk) {
    float ibiS = (float)(bn - m_lastBeatN) + (off - m_lastOff);
    float ibi = ibiS * 1000.0f / m_fs;
    ibiMs = (uint16_t)dspClamp(ibi, 0, 65535);
    bool inRange = ibi >= 60000.0f / P.hrSearchHi && ibi <= 60000.0f / P.hrSearchLo;
    bool ok = inRange;
    if (ok && m_ibiMedN >= 3) {
      float tmp[8];
      float med = dspMedian(m_ibiMed, m_ibiMedN, tmp);
      if (fabsf(ibi - med) > 0.30f * med) ok = false;
    }
    if (m_hrArtNow) {
      // v1.3: strong breathing movement is in the heart band: the interval is kept for the display but not
      // used for heart rate / HRV, and it does not teach the interval model (it may be a breathing burst)
      fl |= BEAT_ART;
    } else if (ok) {
      m_ibiMed[m_ibiMedIdx] = ibi; m_ibiMedIdx = (m_ibiMedIdx + 1) & 7;
      if (m_ibiMedN < 8) m_ibiMedN++;
      m_rejN = 0;
      fl |= BEAT_VALID;
      m_expRR = 0.8f * m_expRR + 0.2f * ibiS;
    } else if (inRange) {
      // a sustained change of rhythm must not be rejected forever
      m_rejBuf[m_rejN & 3] = ibi; m_rejN++;
      if (m_rejN >= 4) {
        float mn = m_rejBuf[0], mx = m_rejBuf[0], sum = 0;
        for (int i = 0; i < 4; i++) { mn = fminf(mn, m_rejBuf[i]); mx = fmaxf(mx, m_rejBuf[i]); sum += m_rejBuf[i]; }
        if ((mx - mn) < 0.10f * (sum / 4.0f)) {
          m_ibiMedN = 0; m_ibiMedIdx = 0;
          for (int i = 0; i < 4; i++) { m_ibiMed[m_ibiMedIdx] = m_rejBuf[i]; m_ibiMedIdx = (m_ibiMedIdx + 1) & 7; m_ibiMedN++; }
          m_expRR = (sum / 4.0f) * m_fs / 1000.0f;
        }
        m_rejN = 0;
      }
    }
  } else {
    fl |= BEAT_FIRST;
  }
  m_lastBeatN = bn; m_lastOff = off; m_haveBeat = true; m_chainOk = true;

  BeatEvent be;
  // beat time = envelope peak minus the moving-average group delay (7.5 samples)
  be.n = bn >= 8 ? bn - 8 : 0;
  be.ibiMs = ibiMs;
  be.flags = fl;
  be.amp = (uint8_t)dspClamp(val * 20.0f, 0, 255);
  beats.push(be);
}

// =====================================================================
//  Analysis (~1 Hz, analysis task)
// =====================================================================
float VitalsEngine::acfPeak(const float* x, int n, int lagLo, int lagHi, float prevLag, float* outLag) {
  *outLag = 0;
  if (lagLo < 2) lagLo = 2;
  if (lagHi > n / 2) lagHi = n / 2;
  if (lagHi > 118) lagHi = 118;
  if (lagHi <= lagLo + 2) return 0;
  float r0 = 0;
  for (int i = 0; i < n; i++) r0 += x[i] * x[i];
  if (r0 <= 1e-12f) return 0;
  r0 /= (float)n;
  float r[122];
  for (int L = lagLo - 1; L <= lagHi + 1; L++) {
    float s = 0;
    const int m = n - L;
    for (int i = 0; i < m; i++) s += x[i] * x[i + L];
    r[L - (lagLo - 1)] = (s / (float)m) / r0;
  }
  auto R = [&](int L) { return r[L - (lagLo - 1)]; };
  float rmax = 0;
  for (int L = lagLo; L <= lagHi; L++)
    if (R(L) > R(L - 1) && R(L) >= R(L + 1) && R(L) > rmax) rmax = R(L);
  if (rmax < 0.1f) return fmaxf(rmax, 0.0f);
  int pick = -1;
  for (int L = lagLo; L <= lagHi; L++)       // earliest strong peak avoids 2x period errors
    if (R(L) > R(L - 1) && R(L) >= R(L + 1) && R(L) >= 0.8f * rmax) { pick = L; break; }
  if (prevLag > 0) {                        // continuity with the previous estimate
    float best = 1e9f;
    for (int L = lagLo; L <= lagHi; L++) {
      if (R(L) > R(L - 1) && R(L) >= R(L + 1) && R(L) >= 0.65f * rmax) {
        float d = fabsf((float)L - prevLag);
        if (d <= 0.12f * prevLag && d < best) { best = d; pick = L; }
      }
    }
  }
  if (pick < 0) return 0;
  // v1.3: never stay locked on TWICE the true period. After a burst of artefacts the continuity rule above could
  // follow the second peak (every other beat) for minutes and report half the heart rate. A peak at about half the
  // chosen lag that is almost as strong is the true period (an S2-only peak is far weaker).
  if (pick >= 2 * lagLo) {
    const int half = (int)lroundf(0.5f * (float)pick);
    int hb = -1; float hv = 0;
    for (int L = half - 2; L <= half + 2; L++) {
      if (L < lagLo || L > lagHi) continue;
      if (R(L) > R(L - 1) && R(L) >= R(L + 1) && R(L) > hv) { hv = R(L); hb = L; }
    }
    if (hb > 0 && hv >= 0.85f * R(pick)) pick = hb;
  }
  float off = dspParabolic(R(pick - 1), R(pick), R(pick + 1));
  *outLag = (float)pick + off;
  return R(pick);
}

void VitalsEngine::analyzeHeart(VitalsResults& r, uint32_t n, uint32_t nowMs) {
  const AgeProfile& P = *m_prof;
  const float fs50 = m_fs / VE_ENV_DIV;

  // ---- motion fraction over the last 60 s (10 Hz motion bits)
  {
    uint32_t n10 = m_n10;
    uint32_t cnt = n10 < 600 ? n10 : 600, mot = 0;
    for (uint32_t i = n10 - cnt; i < n10; i++) mot += motionBit10(i);
    r.motionFrac60 = cnt ? (float)mot / cnt : 1.0f;
  }

  // ---- periodicity (ACF) of each channel and of the fused envelope
  const uint32_t eh = m_envHead;
  int avail = eh < (uint32_t)VE_ENV_LEN ? (int)eh : VE_ENV_LEN;
  int N = avail < 500 ? avail : 500;       // 10 s window
  float qa = 0, qg = 0, qf = 0, lagF = 0, art = 0;
  if (N >= 200) {
    int mot = 0; float ma = 0, mg = 0;
    for (int i = 0; i < N; i++) {
      uint32_t idx = (eh - N + i) % VE_ENV_LEN;
      m_bufA[i] = m_envA[idx]; m_bufB[i] = m_envG[idx]; mot += m_envMotion[idx];
      ma += m_bufA[i]; mg += m_bufB[i];
    }
    ma /= N; mg /= N;
    if (mot < N / 5) {
      // channel quality at 25 Hz (cheap)
      int N25 = N / 2;
      float* a25 = m_scratch; float* g25 = m_scratch + 256;
      for (int i = 0; i < N25; i++) {
        a25[i] = 0.5f * (m_bufA[2 * i] + m_bufA[2 * i + 1]) - ma;
        g25[i] = 0.5f * (m_bufB[2 * i] + m_bufB[2 * i + 1]) - mg;
      }
      int lo25 = (int)(fs50 * 0.5f * 60.0f / P.hrSearchHi), hi25 = (int)(fs50 * 0.5f * 60.0f / P.hrSearchLo) + 1;
      float lagTmp;
      qa = fmaxf(0, acfPeak(a25, N25, lo25, hi25, 0, &lagTmp));
      qg = fmaxf(0, acfPeak(g25, N25, lo25, hi25, 0, &lagTmp));
      // fusion weights ~ quality^2
      float qa2 = dspSq(fmaxf(qa, 0.05f)), qg2 = dspSq(fmaxf(qg, 0.05f));
      float wa = dspClamp(qa2 / (qa2 + qg2), 0.1f, 0.9f);
      float nwa = 0.7f * m_wA + 0.3f * wa;
      m_wA = nwa; m_wG = 1.0f - nwa;
      // fused envelope at 50 Hz
      float mf = 0;
      for (int i = 0; i < N; i++) { m_bufC[i] = m_wA * m_bufA[i] + m_wG * m_bufB[i]; mf += m_bufC[i]; }
      mf /= N;
      for (int i = 0; i < N; i++) m_bufC[i] -= mf;
      int lo = (int)(fs50 * 60.0f / P.hrSearchHi), hi = (int)(fs50 * 60.0f / P.hrSearchLo) + 1;
      qf = fmaxf(0, acfPeak(m_bufC, N, lo, hi, m_prevLag, &lagF));
      // ---- v1.3: breathing artefact check. Fast or forceful breathing shakes the chest in the heart band
      // (air flow, breathing muscles). Such energy rises and falls with the chest rotation speed, a heartbeat
      // does not. When the heart envelope follows the rotation speed, the beats can be breaths (v1.2 then
      // showed the breathing rhythm, or twice it, as the heart rate).
      {
        float ms = 0;
        for (int i = 0; i < N; i++) { m_bufB[i] = m_spd[(eh - N + i) % VE_ENV_LEN]; ms += m_bufB[i]; }
        ms /= N;
        double sd = 0, se = 0;
        for (int i = 0; i < N; i++) { m_bufB[i] -= ms; sd += (double)m_bufB[i] * m_bufB[i]; se += (double)m_bufC[i] * m_bufC[i]; }
        float best = 0;
        const float spdRms = sqrtf((float)(sd / N));
        if (spdRms > 0.4f && sd > 0 && se > 0) {
          const double den = sqrt(sd * se);
          for (int L = -6; L <= 8; L += 2) {             // the envelope lags the rotation speed by 0-0.15 s
            double s = 0;
            for (int i = 0; i < N; i++) { int j = i + L; if (j < 0 || j >= N) continue; s += (double)m_bufC[j] * m_bufB[i]; }
            float c = (float)(s / den);
            if (c > best) best = c;
          }
        }
        art = best;
      }
    }
  }
  m_artEma = 0.6f * m_artEma + 0.4f * art;
  const bool artNow = m_artEma > 0.32f;
  m_hrArtNow = artNow;
  r.brArt = m_artEma; r.hrArtefact = artNow;
  r.qa = qa; r.qg = qg; r.qf = qf; r.wa = m_wA; r.wg = m_wG;
  r.pulseSnr = m_npk > 1e-6f ? m_spk / m_npk : 0;
  if (qf > 0.25f && lagF > 0) {
    r.hrAcf = 60.0f * fs50 / lagF;
    if (qf > 0.35f && !artNow) {                       // v1.3: an artefact rhythm must not teach the beat detector
      m_prevLag = lagF;
      m_expRR = 0.7f * m_expRR + 0.3f * (lagF * VE_ENV_DIV);
    }
    if (!artNow) {
      m_acfHist[m_acfHistIdx] = r.hrAcf; m_acfHistIdx = (m_acfHistIdx + 1) % 60;
      if (m_acfHistN < 60) m_acfHistN++;
    }
  } else {
    r.hrAcf = NAN;
    if (qf < 0.15f) m_prevLag = 0;
  }
  float hrAcfMed = NAN;
  if (m_acfHistN >= 5) { float tmp[64]; hrAcfMed = dspMedian(m_acfHist, m_acfHistN, tmp); }

  // ---- beat statistics over the last 60 s
  {
    const uint32_t bh = beats.head, bo = beats.oldest();
    const uint32_t win = (uint32_t)(60.0f * m_fs);
    const uint32_t nLim = n > win ? n - win : 0;
    // walk back to the first beat of the window
    uint32_t first = bh;
    while (first > bo && beats.at(first - 1).n >= nLim) first--;
    int cnt = 0, nv = 0, nAll = 0;
    double sum = 0, sum2 = 0, sAll = 0, sdAll = 0; int nsdAll = 0;
    double sd2 = 0; int nsd = 0, nn50 = 0;
    float mn = 1e9f, mx = 0, prevValid = -1, prevAll = -1;
    uint32_t lastValidN = 0;
    float* ibis = m_bufA; int nIbi = 0;
    for (uint32_t i = first; i < bh; i++) {
      const BeatEvent& b = beats.at(i);
      cnt++;
      bool inRange = b.ibiMs > 0 && !(b.flags & (BEAT_FIRST | BEAT_ART)) && b.ibiMs >= 60000.0f / P.hrSearchHi && b.ibiMs <= 60000.0f / P.hrSearchLo;
      if (inRange) {
        nAll++; sAll += b.ibiMs;
        if (prevAll > 0) { sdAll += dspSq(b.ibiMs - prevAll); nsdAll++; }
        prevAll = b.ibiMs;
      } else prevAll = -1;
      if (b.flags & BEAT_VALID) {
        float v = b.ibiMs;
        lastValidN = b.n;
        nv++; sum += v; sum2 += (double)v * v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (prevValid > 0) { float d = v - prevValid; sd2 += d * d; nsd++; if (fabsf(d) > 50.0f) nn50++; }
        prevValid = v;
        if (nIbi < 640) ibis[nIbi++] = v;
      } else prevValid = -1;
    }
    r.beats60 = cnt; r.valid60 = nv;
    float windowMs = (float)(nowMs < 60000 ? nowMs : 60000);
    r.coverage = windowMs > 0 ? dspClamp((float)(sum / windowMs), 0, 1) : 0;
    if (nv >= 5) {
      float mean = (float)(sum / nv);
      r.hrBeats = 60000.0f / mean;
      r.meanNN = mean;
      float var = (float)(sum2 / nv - (double)mean * mean);
      r.sdnn = var > 0 ? sqrtf(var) : 0;
      r.rmssd = nsd >= 3 ? sqrtf((float)(sd2 / nsd)) : NAN;
      r.pnn50 = nsd >= 3 ? 100.0f * nn50 / nsd : NAN;
      r.hrMin = 60000.0f / mx; r.hrMax = 60000.0f / mn;
    } else {
      r.hrBeats = NAN; r.meanNN = NAN; r.sdnn = NAN; r.rmssd = NAN; r.pnn50 = NAN; r.hrMin = NAN; r.hrMax = NAN;
    }
    if (nv < 20 || r.coverage < 0.6f) { r.sdnn = NAN; r.rmssd = NAN; r.pnn50 = NAN; }
    // instantaneous HR: median of the last 5 valid intervals (if recent)
    r.hrInst = NAN;
    if (nIbi >= 3 && bh > 0) {
      uint32_t lastN = lastValidN;                 // v1.3: the last ACCEPTED beat must be recent (not any beat marker)
      if (lastN && (float)(n - lastN) < 4.0f * m_fs) {
        int k = nIbi < 5 ? nIbi : 5; float tmp[8];
        r.hrInst = 60000.0f / dspMedian(ibis + nIbi - k, k, tmp);
      }
    }
    if (isnan(r.hrInst) && qf > 0.4f && !artNow) r.hrInst = r.hrAcf;
    // irregular rhythm hint (normalised RMSSD over all in-range intervals)
    bool irregular = false;
    if (nAll >= 30 && nsdAll >= 20) {
      float nr = sqrtf((float)(sdAll / nsdAll)) / (float)(sAll / nAll);
      float validFrac = (float)nv / (float)nAll;
      irregular = nr > 0.12f && m_spk > 2.5f * m_npk && validFrac < 0.85f && r.motionFrac60 < 0.2f;
    }
    if (irregular) r.pattern |= PF_IRREG_RHYTHM; else r.pattern &= ~PF_IRREG_RHYTHM;
  }

  // ---- spectral HR (60 s fused envelope at 12.5 Hz, 1024-pt FFT)
  if ((m_analyzeCount & 1) == 0) {
    const uint32_t sh = m_envSHead;
    int availS = sh < (uint32_t)VE_ENVS_LEN ? (int)sh : VE_ENVS_LEN;
    int NS = availS < 750 ? availS : 750;
    if (NS >= 250) {
      float mean = 0;
      for (int i = 0; i < NS; i++) { m_fftRe[i] = m_envS[(sh - NS + i) % VE_ENVS_LEN]; mean += m_fftRe[i]; }
      mean /= NS;
      for (int i = 0; i < DSP_FFT_MAX; i++) {
        m_fftRe[i] = i < NS ? (m_fftRe[i] - mean) * hannN(i, NS) : 0.0f;
        m_fftIm[i] = 0;
      }
      DspFFT::fft(m_fftRe, m_fftIm, DSP_FFT_MAX);
      for (int k = 0; k <= DSP_FFT_MAX / 2; k++) m_pow[k] = m_fftRe[k] * m_fftRe[k] + m_fftIm[k] * m_fftIm[k];
      const float fsS = m_fs / VE_ENVS_DIV;
      const float df = fsS / DSP_FFT_MAX;
      int kLo = (int)ceilf(P.hrSearchLo / 60.0f / df), kHi = (int)floorf(P.hrSearchHi / 60.0f / df);
      if (kHi > DSP_FFT_MAX / 2 - 2) kHi = DSP_FFT_MAX / 2 - 2;
      int best = -1; float bestS = 0; double meanP = 0;
      for (int k = kLo; k <= kHi; k++) {
        float S = m_pow[k];
        if (2 * k <= DSP_FFT_MAX / 2) S += 0.5f * m_pow[2 * k];
        if (3 * k <= DSP_FFT_MAX / 2) S += 0.33f * m_pow[3 * k];
        if (S > bestS) { bestS = S; best = k; }
        meanP += m_pow[k];
      }
      meanP /= (kHi - kLo + 1);
      if (best > kLo && best < kHi) {
        // refine on the local maximum of P near the harmonic-sum winner
        int k = best;
        if (m_pow[k + 1] > m_pow[k]) k++;
        else if (m_pow[k - 1] > m_pow[k]) k--;
        float off = dspParabolic(m_pow[k - 1], m_pow[k], m_pow[k + 1]);
        r.hrSpec = (k + off) * df * 60.0f;
        r.specProm = meanP > 0 ? (float)(m_pow[k] / meanP) : 0;
      } else { r.hrSpec = NAN; r.specProm = 0; }
      // display spectrum 0-4 Hz
      int k4 = (int)(4.0f / df);
      if (k4 > DSP_FFT_MAX / 2) k4 = DSP_FFT_MAX / 2;
      int kMinNorm = (int)(0.5f / df);
      float mxv = 1e-12f;
      for (int k = kMinNorm; k <= k4; k++) if (m_pow[k] > mxv) mxv = m_pow[k];
      float step = (float)k4 / VE_SPEC_CARD_N;
      uint16_t tmpCard[VE_SPEC_CARD_N];
      for (int j = 0; j < VE_SPEC_CARD_N; j++) {
        int a = (int)(j * step), b = (int)((j + 1) * step);
        if (b <= a) b = a + 1;
        float m = 0;
        for (int k = a; k < b && k <= k4; k++) if (m_pow[k] > m) m = m_pow[k];
        tmpCard[j] = (uint16_t)dspClamp(1000.0f * sqrtf(m / mxv), 0, 1000);
      }
      VE_LOCK();
      memcpy(m_spec.card, tmpCard, sizeof(tmpCard));
      m_spec.cardDf = step * df;
      m_spec.cardPeakHz = isnan(r.hrSpec) ? 0 : r.hrSpec / 60.0f;
      m_spec.version++;
      VE_UNLOCK();
    }
  }

  // ---- v1.3: half / double rate guard. If the spectral and the autocorrelation rate agree with each other and
  // the beat detector runs at about half or double of them for 5 s, the beat detector is re-seeded from them.
  {
    const float hrS0 = r.hrSpec, hrA0 = r.hrAcf, hrB0 = r.hrBeats;
    bool odd = false;
    if (!artNow && !isnan(hrS0) && !isnan(hrA0) && !isnan(hrB0) && r.specProm > 3.0f && qf > 0.35f && fabsf(hrS0 - hrA0) / hrS0 < 0.08f) {
      const float ratio = hrB0 / hrS0;
      odd = fabsf(ratio - 0.5f) < 0.08f || fabsf(ratio - 2.0f) < 0.25f;
    }
    m_subHarmN = odd ? m_subHarmN + 1 : 0;
    if (m_subHarmN >= 5) {
      m_reseedRR = 60.0f * m_fs / hrS0;
      m_prevLag = 60.0f * fs50 / hrS0;
      m_acfHistN = 0; m_acfHistIdx = 0;
      m_subHarmN = 0;
    }
  }
  // ---- fusion of the three HR estimators + signal quality index
  float agree = 0, hr = NAN;
  const float hrB = r.hrBeats, hrS = r.hrSpec;
  if (!isnan(hrB) && r.coverage >= 0.5f) {
    if (!isnan(hrS) && fabsf(hrB - hrS) / hrS < 0.08f) agree = 1.0f;
    else if (!isnan(hrAcfMed) && fabsf(hrB - hrAcfMed) / hrAcfMed < 0.10f) agree = 0.8f;
    else agree = 0.4f;
    hr = hrB;
  } else if (artNow) {
    // v1.3: during breathing artefacts the spectral / autocorrelation rates may be the breathing rhythm: no fallback
  } else if (!isnan(hrS) && r.specProm > 4.0f && !isnan(hrAcfMed) && fabsf(hrS - hrAcfMed) / hrAcfMed < 0.10f) {
    hr = hrS; agree = 0.7f;
  } else if (!isnan(hrAcfMed) && qf > 0.4f) {
    hr = hrAcfMed; agree = 0.4f;
  }
  if (artNow) agree = fminf(agree, 0.3f);
  // Presence gate: a real heart signal is either periodic (envelope ACF) or
  // clearly pulsatile (beat peaks well above the noise peaks; this keeps
  // irregular rhythms such as AF detectable). Pure sensor noise gives
  // qf < 0.25 and SNR < 1.5, real recordings qf > 0.6 and SNR > 3.
  const bool presentNow = (qf >= 0.35f) || (r.pulseSnr >= 2.4f && r.motionFrac60 < 0.5f);
  m_presence = 0.7f * m_presence + 0.3f * (presentNow ? 1.0f : 0.0f);
  r.heartPresent = m_presence > 0.5f;
  float sqi = 100.0f * (0.35f * fminf(1.0f, qf / 0.6f) + 0.35f * fminf(1.0f, r.coverage / 0.9f) + 0.30f * agree) * (1.0f - r.motionFrac60);
  if (!r.heartPresent) sqi = fminf(sqi, 10.0f);
  if (artNow) sqi = fminf(sqi, 35.0f);  // v1.3: below the alarm level while breathing disturbs the heart signal
  r.sqiH = (uint8_t)dspClamp(sqi, 0, 100);
  if (!r.heartPresent) {                 // v1.2: low quality alone no longer blanks the values (they are flagged by the quality index instead)
    r.hr1m = NAN; r.hrInst = NAN; r.hrMin = NAN; r.hrMax = NAN;
    r.sdnn = NAN; r.rmssd = NAN; r.pnn50 = NAN; r.meanNN = NAN;
    r.pattern &= ~PF_IRREG_RHYTHM;
  } else {
    r.hr1m = hr;
    if (!artNow && !isnan(hr) && sqi >= 40.0f) m_lastGoodHr = hr;
  }
  if (artNow) r.pattern &= ~PF_IRREG_RHYTHM;
}

void VitalsEngine::analyzeResp(VitalsResults& r, uint32_t nowMs, bool doSpectrum) {
  const AgeProfile& P = *m_prof;
  const float fs10 = m_fs / VE_RESP_DIV;
  const uint32_t n10 = m_n10;
  const uint32_t lim5 = n10 > (uint32_t)(300 * fs10) ? n10 - (uint32_t)(300 * fs10) : 0;
  const uint32_t lim1 = n10 > (uint32_t)(60 * fs10) ? n10 - (uint32_t)(60 * fs10) : 0;
  r.windowSec = fminf(300.0f, n10 / fs10);

  // motion fraction over 5 min
  float motionFrac5;
  {
    uint32_t cnt = n10 - lim5, mot = 0;
    for (uint32_t i = lim5; i < n10; i++) mot += motionBit10(i);
    motionFrac5 = cnt ? (float)mot / cnt : 1.0f;
  }

  // ---- breath-by-breath statistics
  const uint32_t bh = breaths.head, bo = breaths.oldest();
  const uint32_t lim40 = n10 > (uint32_t)(40 * fs10) ? n10 - (uint32_t)(40 * fs10) : 0;
  int c5 = 0, c1 = 0, cNow = 0, sighs = 0;
  bool nowDone = false;
  double sPer = 0, sPer2 = 0, sAmp = 0, sAmp2 = 0, sTi = 0, sTe = 0, sAmp1 = 0;
  float* per1 = m_bufC;
  double effMs = 0;   // observed (movement-free) time tiled by breath cycles
  double eff1Ms = 0, effNowMs = 0;  // v1.3: the same for the last 60 s, and for the newest 4-8 breaths (>= 20 s)
  for (uint32_t i = bh; i > bo;) {
    i--;
    const BreathEvent& b = breaths.at(i);
    if (b.n10 < lim5) break;
    if (b.n10 < lim40) nowDone = true;
    if (!(b.flags & BR_MOTION)) {
      effMs += b.periodMs;
      if (b.n10 >= lim1) eff1Ms += b.periodMs;
      if (!nowDone) effNowMs += b.periodMs;
    }
    if (!(b.flags & BR_VALID)) continue;
    float per = b.periodMs, amp = b.ampMdeg * 0.001f;
    c5++; sPer += per; sPer2 += (double)per * per; sAmp += amp; sAmp2 += (double)amp * amp;
    sTi += b.tiMs; sTe += b.teMs;
    if (b.flags & BR_SIGH) sighs++;
    if (b.n10 >= lim1 && c1 < 600) { per1[c1++] = per; sAmp1 += amp; }
    if (!nowDone) {
      cNow++;
      if (cNow >= 8 || (cNow >= 4 && effNowMs >= 20000.0)) nowDone = true;
    }
  }
  r.breaths5 = c5; r.sighs5 = sighs;
  r.b1m = c1; r.breathTotal = m_brTotal;
  if (c5 >= 3) {
    float mp = (float)(sPer / c5), ma = (float)(sAmp / c5);
    // Clinical breathing rate = breaths counted per minute of observable
    // (movement-free) time, so apnoeic pauses correctly lower the rate.
    // Before one minute of data exists, fall back to the mean period.
    float effSec = (float)(effMs / 1000.0) + (m_inPause ? (n10 - m_lastConfN) / fs10 : 0.0f);
    r.rr5m = (effSec >= 60.0f) ? 60.0f * c5 / effSec : 60000.0f / mp;
    float vp = (float)(sPer2 / c5 - (double)mp * mp), va = (float)(sAmp2 / c5 - (double)ma * ma);
    r.periodCV = vp > 0 ? sqrtf(vp) / mp : 0;
    r.ampMean = ma; r.ampCV = (va > 0 && ma > 0) ? sqrtf(va) / ma : 0;
    r.tiMean = (float)(sTi / c5) / 1000.0f; r.teMean = (float)(sTe / c5) / 1000.0f;
    r.ieRatio = r.teMean > 0 ? r.tiMean / r.teMean : NAN;
  } else {
    r.rr5m = NAN; r.periodCV = NAN; r.ampMean = NAN; r.ampCV = NAN; r.tiMean = NAN; r.teMean = NAN; r.ieRatio = NAN;
  }
  // v1.3: the 1-minute and 30-second rates COUNT breaths per minute of observed (movement-free) breathing time,
  // like a nurse counting for a minute. v1.2 took the median breath length of the last minute, which stayed at
  // the fast value for up to a minute after a burst of fast breathing (more short breaths than long ones).
  const float pauseNow = m_inPause ? (n10 - m_lastConfN) / fs10 : 0.0f;
  if (c1 >= 2 && eff1Ms > 0) {
    float eff = (float)(eff1Ms / 1000.0) + pauseNow;
    r.rr1m = 60.0f * c1 / fmaxf(eff, 6.0f);
    r.amp1m = (float)(sAmp1 / c1);
  } else { r.rr1m = NAN; r.amp1m = NAN; }
  // "now": the newest 8 breaths (at least 4 and 20 s when breathing slowly), so a change of pace shows within a few breaths
  if (cNow >= 3 && effNowMs > 0) {
    float eff = (float)(effNowMs / 1000.0) + pauseNow;
    r.rrNow = 60.0f * cNow / fmaxf(eff, 3.0f);
  } else if (!isnan(r.rr1m)) r.rrNow = r.rr1m;
  else r.rrNow = NAN;
  (void)per1;

  // ---- pauses / apnea events in 5 min
  {
    int cnt = 0; float longest = 0;
    uint32_t eh = events.head, eo = events.oldest();
    for (uint32_t i = eh; i > eo;) {
      i--;
      const VEvent& e = events.at(i);
      if (e.tMs + 300000u < nowMs) break;
      if (e.type == EV_PAUSE) { cnt++; longest = fmaxf(longest, e.durMs / 1000.0f); }
    }
    r.pauses5 = cnt; r.longestPause5 = longest;
    r.currentPause = m_inPause ? (n10 - m_lastConfN) / fs10 : 0;
  }

  // ---- Welch spectrum over the 5-minute window
  if (doSpectrum) {
    const uint32_t h = resp.head;
    int avail = h < 3000u ? (int)h : 3000;
    if (avail >= 256) {
      int L = avail >= 512 ? 512 : 256, hop = L / 2;
      int nseg = 1 + (avail - L) / hop;
      for (int k = 0; k <= DSP_FFT_MAX / 2; k++) m_pow[k] = 0;
      int used = 0;
      for (int s = 0; s < nseg; s++) {
        uint32_t start = h - avail + s * hop;
        int mot = 0; float mean = 0;
        for (int i = 0; i < L; i++) {
          bool mb = motionBit10(start + i);
          mot += mb;
          m_fftIm[i] = mb ? 1.0f : 0.0f;          // temporary motion mask
          m_fftRe[i] = resp.at(start + i) * 0.001f;
          if (!mb) mean += m_fftRe[i];
        }
        if (mot > L / 3) continue;
        mean /= (float)(L - mot);
        for (int i = 0; i < DSP_FFT_MAX; i++) {
          // movement samples are replaced by the segment mean (zero after
          // mean removal) so artefacts cannot masquerade as breathing power
          m_fftRe[i] = (i < L && m_fftIm[i] == 0.0f) ? (m_fftRe[i] - mean) * DspFFT::hann(i, L) : 0.0f;
          m_fftIm[i] = 0;
        }
        DspFFT::fft(m_fftRe, m_fftIm, DSP_FFT_MAX);
        for (int k = 0; k <= DSP_FFT_MAX / 2; k++) m_pow[k] += m_fftRe[k] * m_fftRe[k] + m_fftIm[k] * m_fftIm[k];
        used++;
      }
      const float df = fs10 / DSP_FFT_MAX;
      if (used > 0) {
        int kLo = (int)ceilf(P.rrSearchLo / 60.0f / df), kHi = (int)floorf(P.rrSearchHi / 60.0f / df);
        if (kHi > DSP_FFT_MAX / 2 - 2) kHi = DSP_FFT_MAX / 2 - 2;
        int best = -1; float bv = 0; double meanP = 0;
        for (int k = kLo; k <= kHi; k++) { if (m_pow[k] > bv) { bv = m_pow[k]; best = k; } meanP += m_pow[k]; }
        meanP /= (kHi - kLo + 1);
        if (best > kLo && best < kHi) {
          float off = dspParabolic(m_pow[best - 1], m_pow[best], m_pow[best + 1]);
          r.rrSpec = (best + off) * df * 60.0f;
          r.rrSpecProm = meanP > 0 ? (float)(bv / meanP) : 0;
        } else { r.rrSpec = NAN; r.rrSpecProm = 0; }
        // display 0-1.25 Hz
        float mxv = 1e-12f;
        int kMin = (int)(0.05f / df);
        for (int k = kMin; k < VE_SPEC_RESP_N; k++) if (m_pow[k] > mxv) mxv = m_pow[k];
        uint16_t tmpResp[VE_SPEC_RESP_N];
        for (int k = 0; k < VE_SPEC_RESP_N; k++) tmpResp[k] = (uint16_t)dspClamp(1000.0f * sqrtf(m_pow[k] / mxv), 0, 1000);
        VE_LOCK();
        memcpy(m_spec.resp, tmpResp, sizeof(tmpResp));
        m_spec.respDf = df;
        m_spec.respPeakHz = isnan(r.rrSpec) ? 0 : r.rrSpec / 60.0f;
        m_spec.version++;
        VE_UNLOCK();
      }
    }
  }

  // ---- presence: valid breaths recently AND a breathing wave clearly above
  // the sensor noise floor (integrated gyro noise can fake a spectral peak)
  r.respLevel = 2.6f * sqrtf(m_rmsL);
  r.respPresent = (r.respLevel >= 0.08f) && (c1 >= 2 || c5 >= 4);
  // ---- fusion + quality
  float agree = 0, rr = NAN;
  if (!isnan(r.rr5m) && c5 >= 5) {
    agree = (!isnan(r.rrSpec) && fabsf(r.rr5m - r.rrSpec) / r.rrSpec < 0.15f) ? 1.0f : 0.5f;
    rr = r.rr5m;
  } else if (!isnan(r.rrSpec) && r.rrSpecProm > 5.0f) {
    rr = r.rrSpec; agree = 0.5f;
  } else if (!isnan(r.rr5m)) {
    rr = r.rr5m; agree = 0.3f;
  }
  float reg = isnan(r.periodCV) ? 0.0f : fmaxf(0.0f, 1.0f - r.periodCV / 0.5f);
  float sqi = 100.0f * (0.4f * fminf(1.0f, r.rrSpecProm / 8.0f) + 0.3f * reg + 0.3f * agree) * (1.0f - motionFrac5);
  if (!r.respPresent) { sqi = fminf(sqi, 10.0f); rr = NAN; r.rr1m = NAN; r.rrSpec = NAN; r.rrNow = NAN; }
  r.sqiR = (uint8_t)dspClamp(sqi, 0, 100);
  r.rr5m = rr;
  r.polarity = m_polarity;
}

void VitalsEngine::analyzeVibration() {
  const uint32_t h = m_vibHead;
  if (h < (uint32_t)VE_VIB_LEN) return;
  int8_t outA[VE_SPEC_VIB_N], outG[VE_SPEC_VIB_N];
  for (int ch = 0; ch < 2; ch++) {
    const int16_t* src = ch == 0 ? m_vibA : m_vibG;
    for (int k = 0; k <= 256; k++) m_pow[k] = 0;
    for (int s = 0; s < 3; s++) {
      uint32_t start = h - VE_VIB_LEN + s * 256;
      float mean = 0;
      for (int i = 0; i < 512; i++) { m_fftRe[i] = src[(start + i) % VE_VIB_LEN]; mean += m_fftRe[i]; }
      mean /= 512.0f;
      for (int i = 0; i < 512; i++) { m_fftRe[i] = (m_fftRe[i] - mean) * DspFFT::hann(i, 512); m_fftIm[i] = 0; }
      DspFFT::fft(m_fftRe, m_fftIm, 512);
      for (int k = 0; k <= 256; k++) m_pow[k] += m_fftRe[k] * m_fftRe[k] + m_fftIm[k] * m_fftIm[k];
    }
    float mx = 1e-20f;
    for (int k = 1; k < VE_SPEC_VIB_N; k++) if (m_pow[k] > mx) mx = m_pow[k];
    if (ch == 0) {
      float lo = 0, hi = 0;
      for (int k = 1; k < 64; k++) lo += m_pow[k];
      for (int k = 64; k < VE_SPEC_VIB_N; k++) hi += m_pow[k];
      m_vHf = hi / (lo + hi + 1e-20f);
    }
    int8_t* out = ch == 0 ? outA : outG;
    for (int k = 0; k < VE_SPEC_VIB_N; k++) {
      float db = 10.0f * log10f((m_pow[k] + 1e-20f) / mx);
      out[k] = (int8_t)dspClamp(db, -60.0f, 0.0f);
    }
  }
  // ---- absolute amplitudes and cough-like bursts (time domain, 5 s window)
  {
    double sa = 0, sg = 0; int pk = 0;
    for (int i = 0; i < VE_VIB_LEN; i++) {
      int a = m_vibA[i]; sa += (double)a * a; sg += (double)m_vibG[i] * m_vibG[i];
      if (abs(a) > pk) pk = abs(a);
    }
    m_vRmsA = sqrtf((float)(sa / VE_VIB_LEN)) / 100.0f;       // stored as 0.01 mg
    m_vRmsG = sqrtf((float)(sg / VE_VIB_LEN)) / 1000.0f;      // stored as mdps
    m_vPkA = pk / 100.0f;
    // (v1.3: cough bursts are detected in detectCoughs() from dedicated 50 ms frames)
  }
  VE_LOCK();
  memcpy(m_spec.vibA, outA, sizeof(outA));
  memcpy(m_spec.vibG, outG, sizeof(outG));
  m_spec.vibDf = m_fs / 512.0f;
  m_spec.version++;
  VE_UNLOCK();
}

// ---------------------------------------------------------------------
//  v1.3 cough detector (50 ms frames, runs once per second)
//
//  v1.2 counted any isolated vibration peak above 40 mg as a cough, so
//  moving an arm, touching the sensor or shifting in bed was reported as
//  coughing. A cough is different from those in four ways, all required:
//    1. energy in the HIGH band (15-45 Hz): the explosive air flow makes
//       the chest wall ring; limb and body movement stays below ~8 Hz
//    2. that high-band energy is a real part of the burst, not a trace
//       of a big slow movement (high band >= 20 % of the 0.5-8 Hz band)
//    3. it lasts like a cough: 0.15-0.7 s (a tap or knock dies out in
//       about 0.05 s, talking and rubbing go on for seconds)
//    4. it stands alone: quiet before and after (no talking, rubbing,
//       walking or repositioning around it)
// ---------------------------------------------------------------------
void VitalsEngine::detectCoughs(uint32_t nowMs) {
  (void)nowMs;
  const uint32_t head = m_cfHead;
  if (head < 64) return;
  const uint32_t first = head > 128 ? head - 128 + 26 : 26;
  if (m_cfDone < first) m_cfDone = first;
  if (head < 28) return;
  const uint32_t last = head - 28;                 // every candidate needs 1.4 s of frames after it
  // usual high-band level: 40th percentile of the frames in the ring (bursts do not move it)
  {
    const int n = head < 128 ? (int)head : 128;
    float* s = m_scratch;
    for (int i = 0; i < n; i++) s[i] = m_cfH[(head - n + i) & 127];
    dspSort(s, n);
    const float q = s[(int)(0.4f * (n - 1))];
    m_cfBase = m_cfBase <= 0 ? q : 0.8f * m_cfBase + 0.2f * q;
  }
  const float thr = fmaxf(6.0f * m_cfBase, 20.0f);
  auto H = [&](uint32_t k) { return m_cfH[k & 127]; };
  auto L = [&](uint32_t k) { return m_cfL[k & 127]; };
  auto W = [&](uint32_t k) { return m_cfW[k & 127]; };
  for (uint32_t k = m_cfDone; k <= last; k++) {
    const float hk = H(k);
    if (hk < thr || hk < H(k - 1) || hk <= H(k + 1)) continue;
    if (m_coughLastIdx && k < m_coughLastIdx + 8) continue;          // 0.4 s refractory
    // 3. duration of the burst (frames above 30 % of its peak)
    uint32_t a = k, b = k;
    while (a > k - 10 && H(a - 1) >= 0.3f * hk) a--;
    while (b < k + 16 && H(b + 1) >= 0.3f * hk) b++;
    const int w = (int)(b - a + 1);
    if (w < 3 || w > 14) continue;
    // 2. high band against the body-movement band inside the burst
    float lmax = 0;
    for (uint32_t j = a; j <= b; j++) lmax = fmaxf(lmax, L(j));
    if (hk < 0.2f * lmax) continue;
    // 4. quiet before and after (high band, and no sustained fast rotation)
    float pre = 0, post = 0; int np = 0, nq = 0, rot = 0;
    for (uint32_t j = k - 26; j + 3 <= a; j++) { pre += H(j); np++; if (W(j) > 15.0f) rot++; }
    for (uint32_t j = b + 3; j <= k + 26; j++) { post += H(j); nq++; if (W(j) > 15.0f) rot++; }
    if (np < 4 || nq < 4) continue;
    pre /= np; post /= nq;
    if (pre > 0.25f * hk || post > 0.25f * hk) continue;
    if (rot > 12) continue;
    // 1. passed all checks: a cough
    m_coughLastIdx = k;
    m_coughT[m_coughN % 16] = msOfSample(k * 10u);
    m_coughN++; m_coughTot++;
  }
  m_cfDone = last + 1;
}

void VitalsEngine::analyze() {
  if (!m_running) return;
  const uint32_t n = m_n;
  const uint32_t nowMs = msOfSample(n);
  VitalsResults r;
  VE_LOCK(); r = m_res; VE_UNLOCK();
  m_analyzeCount++;
  const AgeProfile& P = *m_prof;

  analyzeHeart(r, n, nowMs);
  analyzeResp(r, nowMs, (m_analyzeCount % 5) == 1);
  if ((m_analyzeCount & 1) == 1) analyzeVibration();
  detectCoughs(nowMs);

  // ---- orientation w.r.t. gravity
  float g0 = m_g[0], g1 = m_g[1], g2 = m_g[2];
  r.g[0] = g0; r.g[1] = g1; r.g[2] = g2;
  r.roll = atan2f(g1, g2) * DSP_RAD2DEG;
  r.pitch = atan2f(-g0, sqrtf(g1 * g1 + g2 * g2)) * DSP_RAD2DEG;
  r.tiltFromCal = m_tilt;
  r.postureChanged = m_postureFlag;
  r.motion = m_motion;
  for (int i = 0; i < 3; i++) { r.vCard[i] = m_vA[i]; r.vGyro[i] = m_vG[i]; r.vResp[i] = m_vR[i]; r.gyroBias[i] = m_bias[i]; r.accBias[i] = m_accB[i]; }
  r.fsActual = m_fs; r.sampleCount = n; r.nowMs = nowMs; r.gapSamples = m_gapSamples; r.ageGroup = P.id;
  r.vibRmsA = m_vRmsA; r.vibRmsG = m_vRmsG; r.vibPkA = m_vPkA; r.vibHf = m_vHf;
  { int c = 0; for (int i = 0; i < 16 && i < m_coughN; i++) if (nowMs - m_coughT[i] <= 30000u) c++; r.cough30 = c; r.coughTotal = m_coughTot; }

  // ---- age-referenced pattern flags
  uint16_t pf = r.pattern & PF_IRREG_RHYTHM;
  if (!isnan(r.hr1m) && r.sqiH >= 40) {
    if (r.hr1m > P.hrHi) pf |= PF_TACHYCARDIA;
    if (r.hr1m < P.hrLo) pf |= PF_BRADYCARDIA;
  }
  if (!isnan(r.rr5m) && r.sqiR >= 40) {
    if (r.rr5m > P.rrHi) pf |= PF_TACHYPNEA;
    if (r.rr5m < P.rrLo) pf |= PF_BRADYPNEA;
  }
  if (!isnan(r.periodCV) && r.periodCV > 0.35f && r.breaths5 >= 8) pf |= PF_IRREG_BREATH;
  if (r.pauses5 > 0 || r.currentPause > 0) pf |= PF_PAUSES;
  if (!isnan(r.amp1m) && !isnan(r.ampMean) && r.breaths5 >= 10 && r.amp1m < 0.5f * r.ampMean) pf |= PF_SHALLOW;
  if (r.sighs5 >= 2) pf |= PF_SIGHS;
  if (r.pauses5 >= 2 && !isnan(r.ampCV) && r.ampCV > 0.5f) pf |= PF_PERIODIC;
  if (!isnan(r.rmssd) && P.rmssdLow > 0 && r.rmssd < P.rmssdLow) pf |= PF_LOW_HRV;
  r.pattern = pf;

  // ---- 1 Hz trend point for the 10-minute log
  TrendPoint tp; memset(&tp, 0, sizeof(tp));
  tp.tMs = nowMs;
  tp.hr10 = isnan(r.hrInst) ? 0 : (uint16_t)dspClamp(r.hrInst * 10.0f, 0, 65535);
  tp.rr10 = isnan(r.rrNow) ? 0 : (uint16_t)dspClamp(r.rrNow * 10.0f, 0, 65535);
  tp.sqiH = r.sqiH; tp.sqiR = r.sqiR;
  tp.flags = (r.motion ? TR_MOTION : 0) | (r.currentPause > 0 ? TR_PAUSE : 0) | (r.postureChanged ? TR_POSTURE : 0);
  tp.tilt = (uint8_t)dspClamp(r.tiltFromCal, 0, 180);
  trend.push(tp);

  VE_LOCK(); m_res = r; VE_UNLOCK();
}

void VitalsEngine::getResults(VitalsResults& out) { VE_LOCK(); out = m_res; VE_UNLOCK(); }
void VitalsEngine::getSpectra(VitalsSpectra& out) { VE_LOCK(); out = m_spec; VE_UNLOCK(); }
