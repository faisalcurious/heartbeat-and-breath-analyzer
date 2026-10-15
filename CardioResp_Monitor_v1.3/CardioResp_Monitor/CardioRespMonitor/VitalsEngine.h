// =====================================================================
//  VitalsEngine.h  -  orientation independent heart / breathing engine
//
//  Pipeline (per 200 Hz IMU sample, see VitalsEngine.cpp for details):
//
//   accel(g) + gyro(dps)
//     |-- gyro bias removal (startup calibration + slow still-time tracking)
//     |-- gravity tracking: gyro propagation + accel correction
//     |      (complementary filter on the unit gravity vector, i.e. the
//     |       sensor attitude w.r.t. earth gravity; works in any placement)
//     |-- linear acceleration = accel - gravity
//     |
//     |-- RESPIRATION: gravity-vector deviation (chest wall tilt)
//     |      4th-order anti-alias LP -> decimate to 10 Hz -> band-pass
//     |      -> adaptive PCA over the 3 axes -> 1-D breathing signal (deg)
//     |      -> breath detector (hysteresis, age limits) + pause/apnea
//     |      -> 5-min Welch spectrum
//     |
//     |-- HEART: seismocardiogram (linear accel) + gyrocardiogram (gyro)
//            8th-order band-pass 5-30 Hz on all 6 axes
//            -> adaptive PCA per sensor -> dominant-axis SCG and GCG
//            -> normalised energy envelopes -> quality-weighted fusion
//            -> adaptive-threshold beat detector with search-back
//            -> IBI validation -> HR (1-min), HRV, ACF and spectral HR
//
//  Threading model on the ESP32:
//     pushSample()  - sensor task only (high priority)
//     analyze()     - analysis task (~1 Hz, heavy maths)
//     get*/copy*    - web task; rings use single-writer indices, the
//                     results/spectra are guarded by a mutex.
// =====================================================================
#pragma once
#include "DspCore.h"
#include "AgeNorms.h"
#include <stdio.h>

#define VE_FS            200      // nominal IMU sample rate (Hz)
#define VE_DISP_DIV      2        // 100 Hz waveform for display / report strip
#define VE_DISP_LEN      1024     // 10.24 s
#define VE_ENV_DIV       4        // 50 Hz envelopes for ACF
#define VE_ENV_LEN       640      // 12.8 s
#define VE_ENVS_DIV      16       // 12.5 Hz envelope for 1-min spectrum
#define VE_ENVS_LEN      800      // 64 s
#define VE_RESP_DIV      20       // 10 Hz respiration
#define VE_RESP_LEN      6144     // 614 s (covers 10-min log + 5-min analysis)
#define VE_VIB_LEN       1024     // 5.12 s raw vibration for 0-50 Hz spectrum
#define VE_BEAT_LEN      2048
#define VE_BREATH_LEN    768
#define VE_TREND_LEN     640
#define VE_EVENT_LEN     64
#define VE_GATE_DELAY    120      // 0.6 s delay so motion gating is known before learning

#define VE_SPEC_CARD_N   164      // 0 - 4 Hz   (0 - 240 bpm)
#define VE_SPEC_RESP_N   128      // 0 - 1.25 Hz (0 - 75 br/min)
#define VE_SPEC_VIB_N    128      // 0 - 50 Hz

// beat flags
#define BEAT_VALID       0x01     // interval accepted as normal-to-normal
#define BEAT_SEARCHBACK  0x02     // found by search-back
#define BEAT_FIRST       0x04     // first beat after a gap (no interval)
#define BEAT_ART         0x08     // v1.3: found while strong breathing movement disturbed the heart signal (not used for rates)
// breath flags
#define BR_VALID         0x01
#define BR_MOTION        0x02
#define BR_SIGH          0x04
// trend flags
#define TR_MOTION        0x01
#define TR_PAUSE         0x02
#define TR_POSTURE       0x04
// events
#define EV_PAUSE         1
#define EV_MOTION        2
#define EV_POSTURE       3
#define EV_RECAL         4
// pattern flags (analysis)
#define PF_TACHYCARDIA   0x0001
#define PF_BRADYCARDIA   0x0002
#define PF_IRREG_RHYTHM  0x0004
#define PF_TACHYPNEA     0x0008
#define PF_BRADYPNEA     0x0010
#define PF_IRREG_BREATH  0x0020
#define PF_PAUSES        0x0040
#define PF_SHALLOW       0x0080
#define PF_SIGHS         0x0100
#define PF_PERIODIC      0x0200
#define PF_LOW_HRV       0x0400

struct CalibData {
  float gyroBias[3] = {0, 0, 0};   // dps
  float accScale = 1.0f;           // multiplies accel so |g| = 1
  float g0[3] = {0, 0, 1};         // unit gravity (sensor frame) at calibration
  float accStd[3] = {0, 0, 0};     // g
  float gyroStd[3] = {0, 0, 0};    // dps
  float gMag = 1.0f;               // measured |g| (1.0 for a perfect sensor)
  bool stillOk = false;
  bool gWarn = false;              // accelerometer offset large (compensated automatically)
  int attempts = 0;
};

struct BeatEvent { uint32_t n; uint16_t ibiMs; uint8_t flags; uint8_t amp; };
struct BreathEvent { uint32_t n10; uint16_t periodMs, tiMs, teMs; int16_t ampMdeg; uint8_t flags; uint8_t pad; };
struct TrendPoint { uint32_t tMs; uint16_t hr10, rr10; uint8_t sqiH, sqiR, flags, tilt; };
struct VEvent { uint32_t tMs; uint32_t durMs; uint8_t type; uint8_t pad[3]; };
struct DispSample { int16_t scg, gcg, env; };   // 0.01 mg, 0.001 dps, 0.01 units

struct VitalsResults {
  // ---- heart
  float hr1m = NAN, hrInst = NAN, hrAcf = NAN, hrSpec = NAN, hrBeats = NAN;
  uint8_t sqiH = 0;
  float meanNN = NAN, sdnn = NAN, rmssd = NAN, pnn50 = NAN, hrMin = NAN, hrMax = NAN;
  int beats60 = 0, valid60 = 0;
  float coverage = 0, qa = 0, qg = 0, qf = 0, wa = 0.5f, wg = 0.5f, specProm = 0;
  float pulseSnr = 0;       // beat detector signal/noise peak ratio
  bool heartPresent = false; // periodic or clearly pulsatile cardiac signal found
  // ---- breathing
  float rr5m = NAN, rr1m = NAN, rrSpec = NAN;
  uint8_t sqiR = 0;
  float ampMean = NAN, ampCV = NAN, periodCV = NAN, tiMean = NAN, teMean = NAN, ieRatio = NAN;
  int breaths5 = 0, pauses5 = 0, sighs5 = 0;
  float longestPause5 = 0, currentPause = 0, amp1m = NAN, rrSpecProm = 0, windowSec = 0;
  int polarity = 1;
  float respLevel = 0;      // long-term breathing wave peak-to-peak (deg)
  bool respPresent = false; // a breathing signal above the noise floor exists
  // ---- orientation / motion
  float g[3] = {0, 0, 1};
  float roll = 0, pitch = 0, tiltFromCal = 0;
  float vCard[3] = {0, 0, 1}, vGyro[3] = {0, 0, 1}, vResp[3] = {1, 0, 0};
  bool motion = false, postureChanged = false;
  float motionFrac60 = 0;
  // ---- misc
  uint16_t pattern = 0;
  float fsActual = VE_FS;
  uint32_t sampleCount = 0, nowMs = 0, gapSamples = 0;
  float gyroBias[3] = {0, 0, 0};
  float accBias[3] = {0, 0, 0};    // tracked accelerometer offset (g)
  uint8_t ageGroup = 6;
  // ---- absolute chest-vibration measures (v1.2; never masked, the page colours them)
  float vibRmsA = NAN;      // RMS of the dominant-axis chest acceleration, 0.5-50 Hz (mg)
  float vibRmsG = NAN;      // RMS of the dominant-axis chest rotation rate (deg/s)
  float vibPkA = NAN;       // peak of the same acceleration in the last 5 s (mg)
  float vibHf = NAN;        // share of vibration power above 25 Hz (0..1): broadband bursts (cough)
  int cough30 = 0;          // cough-like vibration bursts in the last 30 s
  // ---- v1.3
  float rrNow = NAN;        // breathing rate over the last ~30 s (fast response; shown as the main breathing value)
  int b1m = 0;              // valid breaths counted in the last 60 s
  uint32_t breathTotal = 0; // valid breaths since the measurement started
  uint32_t coughTotal = 0;  // cough bursts since the measurement started
  bool hrArtefact = false;  // heart channel disturbed by strong breathing movement (heart rate uncertain)
  float brArt = 0;          // correlation of the heart envelope with the breathing movement (0..1)
};
#define VE_HAS_RRNOW 1

struct VitalsSpectra {
  uint16_t card[VE_SPEC_CARD_N] = {0};   // 0..1000 normalised magnitude
  uint16_t resp[VE_SPEC_RESP_N] = {0};   // 0..1000 normalised magnitude
  int8_t vibA[VE_SPEC_VIB_N] = {0};      // dB rel. max (-60..0)
  int8_t vibG[VE_SPEC_VIB_N] = {0};
  float cardDf = 0, respDf = 0, vibDf = 0;   // Hz per output bin
  float cardPeakHz = 0, respPeakHz = 0;
  uint32_t version = 0;
};

// Generic single-writer ring buffer: head counts total items ever written.
template <typename T, int N>
struct Ring {
  T buf[N];
  volatile uint32_t head = 0;
  void reset() { memset((void*)buf, 0, sizeof(buf)); head = 0; }
  inline void push(const T& v) { buf[head % N] = v; head = head + 1; }
  inline const T& at(uint32_t absIdx) const { return buf[absIdx % N]; }
  inline T& atMut(uint32_t absIdx) { return buf[absIdx % N]; }
  inline uint32_t oldest() const { return head > (uint32_t)N ? head - N : 0; }
  static constexpr int capacity() { return N; }
};


// ---------------------------------------------------------------------
//  Startup calibration statistics (portable; used by the firmware and
//  the PC test). Feed raw physical samples (g, dps) while the wearer is
//  still, then finish() evaluates stillness, gravity magnitude and
//  produces gyro bias, accelerometer scale and the gravity reference.
// ---------------------------------------------------------------------
struct CalibAccumulator {
  double s[6], s2[6];
  float mn[6], mx[6];
  uint32_t n = 0;
  void reset() {
    for (int i = 0; i < 6; i++) { s[i] = 0; s2[i] = 0; mn[i] = 1e9f; mx[i] = -1e9f; }
    n = 0;
  }
  void add(float ax, float ay, float az, float gx, float gy, float gz) {
    float v[6] = {ax, ay, az, gx, gy, gz};
    for (int i = 0; i < 6; i++) {
      s[i] += v[i]; s2[i] += (double)v[i] * v[i];
      if (v[i] < mn[i]) mn[i] = v[i];
      if (v[i] > mx[i]) mx[i] = v[i];
    }
    n++;
  }
  // returns true when the wearer was still enough; msg explains a failure
  bool finish(CalibData& c, char* msg, int msgLen) {
    if (n < 100) { snprintf(msg, msgLen, "Not enough samples"); return false; }
    float mean[6], sd[6];
    for (int i = 0; i < 6; i++) {
      mean[i] = (float)(s[i] / n);
      double v = s2[i] / n - (double)mean[i] * mean[i];
      sd[i] = v > 0 ? (float)sqrt(v) : 0.0f;
    }
    float gm = sqrtf(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    c.gMag = gm;
    for (int i = 0; i < 3; i++) {
      c.gyroBias[i] = mean[3 + i];
      c.accStd[i] = sd[i];
      c.gyroStd[i] = sd[3 + i];
      c.g0[i] = gm > 0.1f ? mean[i] / gm : (i == 2 ? 1.0f : 0.0f);
    }
    // Accelerometer offsets are handled by the engine (bias tracking along
    // gravity), so |g| far from 1 only produces a warning. Only a reading
    // that cannot be gravity at all rejects the calibration.
    c.accScale = 1.0f;
    c.gWarn = (gm < 0.93f || gm > 1.07f);
    bool ok = true;
    msg[0] = 0;
    float gyroRange = 0;
    for (int i = 3; i < 6; i++) gyroRange = fmaxf(gyroRange, mx[i] - mn[i]);
    if (gm < 0.5f || gm > 1.6f) { snprintf(msg, msgLen, "Gravity reads %.3f g - sensor faulty or wrong range", gm); ok = false; }
    else if (sd[3] > 3.0f || sd[4] > 3.0f || sd[5] > 3.0f || gyroRange > 30.0f) { snprintf(msg, msgLen, "Movement detected (gyro) - keep still"); ok = false; }
    else if (sd[0] > 0.04f || sd[1] > 0.04f || sd[2] > 0.04f) { snprintf(msg, msgLen, "Movement detected (accel) - keep still"); ok = false; }
    else if (fabsf(mean[3]) > 25.0f || fabsf(mean[4]) > 25.0f || fabsf(mean[5]) > 25.0f) { snprintf(msg, msgLen, "Gyro offset unusually large"); ok = false; }
    c.stillOk = ok;
    return ok;
  }
};

class VitalsEngine {
 public:
  void init();                                     // allocate mutex, FFT tables
  void begin(const CalibData& cal, float ageYears);  // (re)start after calibration
  void pushSample(float ax, float ay, float az, float gx, float gy, float gz);  // g, dps
  void pushGap(int missing);                       // fill lost samples (hold last)
  void analyze();                                  // heavy analysis, call ~1 Hz

  // commands (thread safe, applied by the sensor task)
  void setAge(float years) { m_pendingAge = years; m_agePending = true; }
  float age() const { return m_ageYears; }
  void flipPolarity() { m_flipRequest = true; }
  void setFsActual(float fs);
  bool isRunning() const { return m_running; }
  void halt() { m_running = false; }                // stop processing (calibration)
  uint32_t session() const { return m_session; }    // increments at every begin()
  uint32_t respIndex() const { return m_n10; }      // 10 Hz sample counter
  bool motionNow() const { return m_motion; }
  bool respMotionAt(uint32_t n10) const { return motionBit10(n10); }

  // snapshots (thread safe)
  void getResults(VitalsResults& out);
  void getSpectra(VitalsSpectra& out);
  const CalibData& calib() const { return m_cal; }
  const AgeProfile& profile() const { return *m_prof; }

  // time helpers
  uint32_t sampleIndex() const { return m_n; }
  uint32_t msOfSample(uint32_t n) const;           // device ms (since begin)
  uint32_t nowMs() const { return msOfSample(m_n); }
  float msPerSample() const { return 1000.0f / m_fs; }

  // rings (single writer = sensor task)
  Ring<DispSample, VE_DISP_LEN> disp;     // 100 Hz, index = sample/2
  Ring<int16_t, VE_RESP_LEN> resp;        // 10 Hz, mdeg
  Ring<BeatEvent, VE_BEAT_LEN> beats;
  Ring<BreathEvent, VE_BREATH_LEN> breaths;
  Ring<TrendPoint, VE_TREND_LEN> trend;   // written by analyze()
  Ring<VEvent, VE_EVENT_LEN> events;
  uint32_t respPeakN10 = 0;               // last breath peak (for markers)

 private:
  // ---------- per-sample helpers
  void applyPending();
  void designFilters();
  void respStep(const float rl[3], bool motion);
  void breathDetect(float y, uint32_t n10, bool motion);
  void beatStep(float D, bool motion);
  void resetBreathDetector(float y);
  void acceptBeat(uint32_t bn, float val, uint8_t extraFlags);
  void addEvent(uint8_t type, uint32_t tMs, uint32_t durMs);
  bool motionBit10(uint32_t n10) const { return (m_respMotion[(n10 % VE_RESP_LEN) >> 3] >> (n10 & 7)) & 1; }

  // ---------- analysis helpers
  float acfPeak(const float* x, int n, int lagLo, int lagHi, float prevLag, float* outLag);
  void analyzeHeart(VitalsResults& r, uint32_t n, uint32_t nowMs);
  void analyzeResp(VitalsResults& r, uint32_t nowMs, bool doSpectrum);
  void analyzeVibration();
  void detectCoughs(uint32_t nowMs);       // v1.3

  // ---------- configuration / state
  CalibData m_cal;
  const AgeProfile* m_prof = &AGE_TABLE[6];
  volatile float m_ageYears = 30, m_pendingAge = 30;
  volatile bool m_agePending = false, m_flipRequest = false, m_running = false;
  float m_fs = VE_FS;           // measured true sample rate
  double m_baseMs = 0; uint32_t m_baseN = 0;
  uint32_t m_n = 0;             // samples since begin()
  uint32_t m_session = 0;
  float m_lastRaw[6] = {0, 0, 0, 0, 0, 0};

  // orientation
  float m_g[3] = {0, 0, 1};
  float m_bias[3] = {0, 0, 0};
  float m_motG = 0, m_motA = 0;
  // v1.3 motion detector: gravity-free linear acceleration and the part of the rotation that is faster than breathing
  float m_motGh = 0;                      // mean square of the high-passed angular rate (dps^2)
  float m_rotI[3] = {0, 0, 0};            // leaky rotation integral (deg): posture-change detector
  bool m_turning = false;
  float m_hpL[3] = {0, 0, 0}, m_hpLprev[3] = {0, 0, 0};   // 0.5 Hz HP of the linear acceleration (g)
  float m_hpW[3] = {0, 0, 0}, m_hpWprev[3] = {0, 0, 0};   // 1.5 Hz HP of the angular rate (dps)
  Biquad m_spdLP[3];                      // 2.5 Hz LP of the angular rate: chest rotation speed (breathing)
  // v1.3 cough detector: 50 ms frames of high-band (15-45 Hz) and low-band (0.5-8 Hz) chest acceleration
  Biquad m_cfHp[3], m_cfLp[3], m_lfLp[3];
  float m_cfAccH = 0, m_cfAccL = 0, m_cfAccW = 0; int m_cfAccN = 0;
  float m_cfH[128], m_cfL[128], m_cfW[128]; volatile uint32_t m_cfHead = 0;   // 20 Hz frames (6.4 s)
  uint32_t m_cfDone = 0;                  // next frame the cough detector looks at
  float m_cfBase = 0;                     // usual high-band level (mg)
  uint32_t m_coughTot = 0;
  float m_accB[3] = {0, 0, 0};  // tracked accelerometer offset (g)
  float m_magLP = 1.0f;         // slow |a| average (dynamic-acceleration gate)
  float m_hpA[3] = {0, 0, 0}, m_hpPrev[3] = {0, 0, 0};   // 0.5 Hz HP accel for motion
  float m_theta[3] = {0, 0, 0}; // gyro-integrated rotation (deg) = breathing channel
  float m_rmsS = 0, m_rmsL = 0; // breathing signal power, short (8 s) / long (60 s)
  uint32_t m_fastAdaptUntil = 0;
  uint32_t m_motionUntil = 0; bool m_motion = false, m_prevMotion = false;
  uint32_t m_motionStartN = 0;
  uint32_t m_postureN = 0; bool m_postureFlag = false;
  float m_tilt = 0; bool m_startupMotion = true;
  uint32_t m_noBeatN = 0;       // reference for "no beat yet" threshold decay
  float m_lastOff = 0;          // sub-sample offset of last beat
  uint32_t m_lastConfN = 0;     // last breath extreme confirmation (10 Hz idx)

  // cardiac
  BandPass8 m_bpA[3], m_bpG[3];
  Biquad m_vibHpA, m_vibHpG;
  float m_covA[6] = {0}, m_covG[6] = {0};
  float m_dlA[VE_GATE_DELAY][3], m_dlG[VE_GATE_DELAY][3];   // delayed samples for PCA / scaling
  uint32_t m_lastMotionN = 0;
  float m_dlR[8][3]; uint32_t m_lastMotionN10 = 0;
  float m_vA[3] = {0, 0, 1}, m_vG[3] = {0, 0, 1};
  float m_normA = 1e-3f, m_normG = 1e-3f;
  MovingAverage<16> m_maA, m_maG;
  volatile float m_wA = 0.5f, m_wG = 0.5f;
  float m_envAcc = 0; int m_envAccN = 0;
  float m_envA[VE_ENV_LEN], m_envG[VE_ENV_LEN];
  float m_spd[VE_ENV_LEN];                // v1.3: chest rotation speed (deg/s) at 50 Hz, aligned with the envelopes
  uint8_t m_envMotion[VE_ENV_LEN];
  volatile uint32_t m_envHead = 0;
  float m_envS[VE_ENVS_LEN]; volatile uint32_t m_envSHead = 0;
  int16_t m_vibA[VE_VIB_LEN], m_vibG[VE_VIB_LEN]; volatile uint32_t m_vibHead = 0;   // 0.01 mg / mdps

  // beat detector
  float m_dHist[64];
  float m_spk = 2.5f, m_npk = 1.0f;
  bool m_haveBeat = false, m_chainOk = false, m_cand = false;
  uint32_t m_lastBeatN = 0, m_candN = 0, m_bestRejN = 0;
  float m_lastBeatT = 0, m_candVal = 0, m_bestRejVal = 0;
  volatile float m_expRR = 0.8f * VE_FS;  // expected interval (samples)
  float m_ibiMed[8]; int m_ibiMedN = 0, m_ibiMedIdx = 0;
  float m_rejBuf[4]; int m_rejN = 0;
  volatile bool m_hrArtNow = false;       // v1.3: set by the analysis while the heart envelope follows the breathing
  volatile float m_reseedRR = 0;          // v1.3: analysis -> beat detector: restart the expected interval (samples)
  uint32_t m_learnUntil = 0; float m_learnMax = 0, m_learnSum = 0; int m_learnCnt = 0;

  // respiration
  Butter4 m_respAA[3];          // anti-alias LP at 200 Hz
  Biquad m_respHP[3];           // 0.05 Hz HP at 10 Hz
  Butter4 m_respLP[3];          // age dependent LP at 10 Hz
  float m_covR[6] = {0};
  float m_vR[3] = {1, 0, 0};
  int m_polarity = 1; bool m_autoPolarity = true; float m_polScore = 0; int m_polCount = 0;
  uint32_t m_n10 = 0;
  uint8_t m_respMotion[VE_RESP_LEN / 8];
  bool m_primeResp = true;
  // breath detector
  int m_brState = 0;            // 0 = seeking trough, 1 = seeking peak
  float m_brExt = 0; uint32_t m_brExtN = 0;
  bool m_haveTrough = false, m_havePeak = false, m_brMotionSeen = false;
  uint32_t m_troughN = 0, m_peakN = 0, m_lastExtN = 0;
  float m_troughV = 0, m_peakV = 0, m_ampEma = 0.3f;
  int m_brCount = 0;
  bool m_inPause = false;
  float m_brPerEma = 4.0f;                // v1.3: recent breath period (s), for the hysteresis re-learning
  uint32_t m_brTotal = 0;

  // analysis state (analysis task only)
  float m_prevLag = 0;
  float m_presence = 0;         // smoothed heart-signal presence (0..1)
  float m_artEma = 0;           // v1.3: smoothed breathing-artefact correlation of the heart envelope
  float m_lastGoodHr = NAN;     // v1.3: last heart rate measured without breathing artefact
  int m_subHarmN = 0;           // v1.3: seconds the beat detector ran at half / double the spectral rate
  float m_acfHist[64]; int m_acfHistN = 0, m_acfHistIdx = 0;
  float m_bufA[640], m_bufB[640], m_bufC[640];
  float m_fftRe[DSP_FFT_MAX], m_fftIm[DSP_FFT_MAX], m_pow[DSP_FFT_MAX / 2 + 1];
  float m_scratch[512];
  // chest-vibration summary (written by analyzeVibration, copied to the results)
  float m_vRmsA = NAN, m_vRmsG = NAN, m_vPkA = NAN, m_vHf = NAN;
  uint32_t m_coughT[16]; int m_coughN = 0; uint32_t m_coughLastIdx = 0; float m_coughBase = 0;
  uint32_t m_analyzeCount = 0;
  uint32_t m_gapSamples = 0;

  VitalsResults m_res;          // guarded by mutex
  VitalsSpectra m_spec;         // guarded by mutex
  float m_lastRR = NAN, m_lastHR = NAN;
};
