// =====================================================================
//  Analytics.h  -  rule-based severity + tiny ML inference + written result
//
//  Portable C++ (also compiled on the PC by host_test/analytics_test.cpp).
//  The dashboard carries an identical port in web/analytics.js; the
//  feature definition lives in ml/cr_features.py.  Keep all three in sync.
//
//  Pipeline (once per second on the device):
//    VitalsResults -> AnaIn (values rounded like the JSON stream)
//        |-- zone rules      age-referenced heart / breathing zones (mild / moderate / severe)
//        |-- spectral rules  beat-spectrum prominence, estimator disagreement,
//        |                   breathing-spectrum mismatch, coughing
//        |-- event rules     breathing pauses, heart signal lost (arrest), early-deterioration trend
//        |-- long-term rules (v1.3) a minute-by-minute record of the whole session. Patterns that
//        |                   name an illness (chest infection, sleep apnoea, atrial fibrillation,
//        |                   Cheyne-Stokes breathing, body-wide stress / sepsis signs) are only
//        |                   suggested after enough calm monitoring (5-30 min, see ANA_NEED_*),
//        |                   never from a few seconds of data
//        |-- NEWS2 (v1.3)    partial National Early Warning Score (breathing, pulse, temperature)
//        |-- ML              21 features -> 14 hidden (ReLU) -> 7 classes (softmax)
//        '-- fusion          severity = max(rules, ML hint); reliability index;
//                            written result: headline, suspected condition, score,
//                            reliability, evidence, notes on missing inputs
//
//  Not a diagnosis: every text says "suspected" / "pattern compatible with".
// =====================================================================
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "VitalsEngine.h"
#include "MlModel.h"

#define ANA_MAX_EV 8
#define ANA_MAX_MISS 4
#define ANA_TXT 140
#define ANA_LT_N 240              // minutes of history kept for the long-term rules (4 h)
#define ANA_NEED_INFECT 30        // minutes of calm monitoring before a chest-infection pattern can be named
#define ANA_NEED_APNOEA 30        // ... sleep-apnoea pattern
#define ANA_NEED_AF 5             // ... sustained irregular rhythm (possible atrial fibrillation)
#define ANA_NEED_CSR 10           // ... Cheyne-Stokes-like periodic breathing
#define ANA_NEED_SIRS 20          // ... body-wide stress signs (fast heart + fast breathing)

// ---- rounded inputs, exactly the values the dashboard receives -----------------------------
struct AnaIn {
  float hr = NAN, rr = NAN, nn = NAN, rmssd = NAN, sdnn = NAN, pnn50 = NAN, hrmin = NAN, hrmax = NAN;
  float hra = NAN, hrs = NAN, hrb = NAN, sp = 0, pcv = NAN, ampcv = NAN, apl = 0, apc = 0, amp1 = NAN, amp = NAN;
  float ie = NAN, rrs = NAN, rr1 = NAN, vhf = NAN, mf = 0, tbody = NAN;
  int ap5 = 0, cgh = 0, sqh = 0, sqr = 0, sig = 0, v60 = 0, b60 = 0;
  bool pres = false, mot = false, post = false;
  float win = 0;
  // v1.3
  float rrn = NAN;                 // breathing rate, newest breaths
  long ctot = 0;                   // cough bursts since the measurement started
  bool art = false;                // heart signal disturbed by strong breathing movement
  bool tcal = false;               // body temperature calibrated against a thermometer
  int pf = 0;                      // pattern flags of the engine
};

static inline float anaR(float v, int dec) {
  if (isnan(v) || isinf(v)) return NAN;
  float m = 1.0f; for (int i = 0; i < dec; i++) m *= 10.0f;
  return roundf(v * m) / m;
}

inline AnaIn anaFromResults(const VitalsResults& r, float tbodyC, bool tempCalibrated) {
  AnaIn a;
  a.hr = anaR(r.hr1m, 1); a.rr = anaR(r.rr5m, 1); a.nn = anaR(r.meanNN, 0); a.rmssd = anaR(r.rmssd, 1);
  a.sdnn = anaR(r.sdnn, 1); a.pnn50 = anaR(r.pnn50, 1); a.hrmin = anaR(r.hrMin, 1); a.hrmax = anaR(r.hrMax, 1);
  a.hra = anaR(r.hrAcf, 1); a.hrs = anaR(r.hrSpec, 1); a.hrb = anaR(r.hrBeats, 1); a.sp = anaR(r.specProm, 1);
  a.pcv = anaR(r.periodCV, 3); a.ampcv = anaR(r.ampCV, 3); a.apl = anaR(r.longestPause5, 1); a.apc = anaR(r.currentPause, 1);
  a.amp1 = anaR(r.amp1m, 3); a.amp = anaR(r.ampMean, 3); a.ie = anaR(r.ieRatio, 2); a.rrs = anaR(r.rrSpec, 1);
  a.rr1 = anaR(r.rr1m, 1); a.vhf = anaR(r.vibHf, 2); a.mf = anaR(r.motionFrac60, 2); a.tbody = tbodyC;
  a.ap5 = r.pauses5; a.cgh = r.cough30; a.sqh = r.sqiH; a.sqr = r.sqiR; a.sig = r.sighs5; a.v60 = r.valid60; a.b60 = r.beats60;
  a.pres = r.heartPresent; a.mot = r.motion; a.post = r.postureChanged; a.win = anaR(r.windowSec, 0);
  a.rrn = anaR(r.rrNow, 1); a.ctot = (long)r.coughTotal; a.art = r.hrArtefact; a.tcal = tempCalibrated; a.pf = r.pattern;
  return a;
}

static inline bool anaNum(float v) { return !isnan(v) && !isinf(v); }
static inline float anaClip(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- ML ----------------------------------------------------------------------------------
inline void anaFeatures(const AnaIn& s, const AgeProfile& P, float x[ML_N_IN]) {
  for (int i = 0; i < ML_N_IN; i++) x[i] = 0;
  const bool hrOk = s.pres && anaNum(s.hr), rrOk = anaNum(s.rr);
  const float hmid = 0.5f * (P.hrLo + P.hrHi), hhalf = 0.5f * (P.hrHi - P.hrLo);
  const float rmid = 0.5f * (P.rrLo + P.rrHi), rhalf = 0.5f * (P.rrHi - P.rrLo);
  if (hrOk) { x[0] = anaClip((s.hr - hmid) / hhalf, -4, 4) / 2.0f; x[2] = 1; }
  if (rrOk) { x[1] = anaClip((s.rr - rmid) / rhalf, -4, 4) / 2.0f; x[3] = 1; }
  if (hrOk && anaNum(s.nn) && s.nn > 200) {
    if (anaNum(s.rmssd)) x[4] = anaClip(s.rmssd / s.nn, 0, 0.4f) * 5.0f;
    if (anaNum(s.sdnn)) x[5] = anaClip(s.sdnn / s.nn, 0, 0.4f) * 5.0f;
  }
  if (hrOk && anaNum(s.pnn50)) x[6] = anaClip(s.pnn50, 0, 100) / 50.0f;
  if (hrOk && anaNum(s.hrmin) && anaNum(s.hrmax) && s.hr > 0) x[7] = anaClip((s.hrmax - s.hrmin) / s.hr, 0, 1) * 3.0f;
  if (hrOk && s.hr > 0) {
    float d = 0;
    if (anaNum(s.hra)) d = fmaxf(d, fabsf(s.hra - s.hr));
    if (anaNum(s.hrs)) d = fmaxf(d, fabsf(s.hrs - s.hr));
    if (anaNum(s.hrb)) d = fmaxf(d, fabsf(s.hrb - s.hr));
    x[8] = anaClip(d / s.hr, 0, 0.5f) * 4.0f;
  }
  if (hrOk) x[9] = anaClip(s.sp, 0, 12) / 6.0f;
  if (rrOk && anaNum(s.pcv)) x[10] = anaClip(s.pcv, 0, 1) * 3.0f;
  if (rrOk && anaNum(s.ampcv)) x[11] = anaClip(s.ampcv, 0, 1.5f) * 2.0f;
  x[12] = anaClip((float)s.ap5, 0, 6) / 3.0f;
  if (P.apneaSec > 0) x[13] = anaClip(fmaxf(s.apl, s.apc) / P.apneaSec, 0, 2);
  x[14] = 1.0f;
  if (rrOk && anaNum(s.amp1) && anaNum(s.amp) && s.amp > 1e-6f) x[14] = anaClip(s.amp1 / s.amp, 0, 2);
  x[15] = 0.8f;
  if (rrOk && anaNum(s.ie)) x[15] = anaClip(s.ie, 0, 2);
  if (rrOk && s.rr > 0 && anaNum(s.rrs)) x[16] = anaClip(fabsf(s.rrs - s.rr) / s.rr, 0, 0.6f) * 3.0f;
  x[17] = anaClip((float)s.cgh, 0, 10) / 5.0f;
  if (anaNum(s.vhf)) x[18] = anaClip(s.vhf, 0, 1) * 2.0f;
  x[19] = anaClip(s.mf, 0, 1) * 2.0f;
  x[20] = ((float)s.sqh + (float)s.sqr) / 200.0f;
}

inline void anaMl(const float x[ML_N_IN], float p[ML_N_CLASS]) {
  float h[ML_N_HID];
  for (int j = 0; j < ML_N_HID; j++) {
    float a = ML_B1[j];
    for (int i = 0; i < ML_N_IN; i++) a += ML_W1[j][i] * x[i];
    h[j] = a > 0 ? a : 0;
  }
  float z[ML_N_CLASS], m = -1e30f;
  for (int k = 0; k < ML_N_CLASS; k++) {
    float a = ML_B2[k];
    for (int j = 0; j < ML_N_HID; j++) a += ML_W2[k][j] * h[j];
    z[k] = a; if (a > m) m = a;
  }
  float s = 0;
  for (int k = 0; k < ML_N_CLASS; k++) { z[k] = expf(z[k] - m); s += z[k]; }
  for (int k = 0; k < ML_N_CLASS; k++) p[k] = z[k] / s;
}

static const char* const ANA_CLASS[ML_N_CLASS] = {
  "normal", "tachycardia", "bradycardia", "irregular rhythm", "fast / laboured breathing", "apnoea or slow breathing", "coughing"};

// ---- per-second conditions shared by the tracker and the rules -----------------------------
static inline bool anaHrOk(const AnaIn& s) { return s.pres && anaNum(s.hr); }
// irregular-rhythm signature: no clear beat-spectrum peak, estimators disagree, large beat-to-beat variation
static inline bool anaIrregNow(const AnaIn& s) {
  if (!anaHrOk(s) || s.art || s.mot) return false;
  float dis = 0;
  if (anaNum(s.hra)) dis = fmaxf(dis, fabsf(s.hra - s.hr));
  if (anaNum(s.hrs)) dis = fmaxf(dis, fabsf(s.hrs - s.hr));
  if (anaNum(s.hrb)) dis = fmaxf(dis, fabsf(s.hrb - s.hr));
  dis = s.hr > 0 ? dis / s.hr : 0;
  const float rel = (anaNum(s.rmssd) && anaNum(s.nn) && s.nn > 200) ? s.rmssd / s.nn : 0;
  return s.sp < 2.2f && dis > 0.08f && rel > 0.10f;
}
// fast heart AND fast breathing at rest (adult limits 90 / 20 as in the SIRS criteria; children: age range)
static inline bool anaStressNow(const AnaIn& s, const AgeProfile& P) {
  if (!anaHrOk(s) || !anaNum(s.rr) || s.sqh < 40 || s.sqr < 40 || s.mot || s.art) return false;
  const bool adult = P.id >= 6;
  const float hl = adult ? 90.0f : P.hrHi, rl = adult ? 20.0f : P.rrHi;
  return s.hr > hl && s.rr > rl;
}
static inline bool anaFeverNow(const AnaIn& s) { return s.tcal && anaNum(s.tbody) && s.tbody >= 37.8f; }

// ---- minute-by-minute record (v1.3) ---------------------------------------------------------
struct AnaMin {                    // seconds in that minute with each condition, and counts
  uint8_t valid, fever, tachyp, tachy, irreg, periodic, stress, coughs, pauses, pad;
};
struct AnaLt { int n, obs, coughs, coughBlocks, fever, tachyp, tachy, irreg, periodic, stress, pauses; };

// ---- history for event rules (call update() once per second) -------------------------------
struct AnaTracker {
  uint32_t goodHeartMs = 0;        // last time a good heart signal was present
  uint32_t heartGoodSince = 0;     // start of the current good-heart stretch
  uint32_t lostSince = 0;          // start of the current "no heart signal while still" stretch
  uint32_t lastMotionMs = 0;       // last time movement / posture change / breathing artefact was seen
  uint32_t t0 = 0;
  float hrFast = NAN, hrSlow = NAN, rrFast = NAN, rrSlow = NAN;
  bool wasGood = false, wasPost = false;
  // v1.3 long-term record
  uint32_t lastMs = 0, curIdx = 0;
  uint32_t acc[7] = {0, 0, 0, 0, 0, 0, 0};    // ms: valid, fever, tachyp, tachy, irreg, periodic, stress
  uint16_t curCough = 0, curPause = 0;
  long lastCtot = -1;
  bool inPause = false;
  AnaMin hist[ANA_LT_N];
  uint32_t histN = 0;               // completed minutes ever recorded (ring index = histN % ANA_LT_N)
  void reset() {                    // field by field: no 2.5 kB temporary on the task stack
    goodHeartMs = heartGoodSince = lostSince = lastMotionMs = t0 = 0;
    hrFast = hrSlow = rrFast = rrSlow = NAN; wasGood = false; wasPost = false;
    lastMs = 0; curIdx = 0; for (int i = 0; i < 7; i++) acc[i] = 0;
    curCough = curPause = 0; lastCtot = -1; inPause = false;
    memset(hist, 0, sizeof hist); histN = 0;
  }
  void closeMinute() {
    AnaMin m; memset(&m, 0, sizeof m);
    uint8_t* f[7] = {&m.valid, &m.fever, &m.tachyp, &m.tachy, &m.irreg, &m.periodic, &m.stress};
    for (int i = 0; i < 7; i++) { uint32_t s = (acc[i] + 500) / 1000; *f[i] = (uint8_t)(s > 60 ? 60 : s); acc[i] = 0; }
    m.coughs = (uint8_t)(curCough > 255 ? 255 : curCough); m.pauses = (uint8_t)(curPause > 255 ? 255 : curPause);
    curCough = 0; curPause = 0;
    hist[histN % ANA_LT_N] = m; histN++;
  }
  void update(const AnaIn& s, const AgeProfile& P, uint32_t nowMs) {
    if (!t0) t0 = nowMs ? nowMs : 1;
    // a posture change counts as movement when it HAPPENS (the flag then stays on while the person keeps the new posture)
    if (s.mot || s.art || (s.post && !wasPost)) lastMotionMs = nowMs;
    wasPost = s.post;
    const bool good = s.pres && anaNum(s.hr) && s.sqh >= 40 && !s.art;
    if (good) {
      // v1.3: a gap of up to 5 s does not end the good stretch (a one-second blip must not hide a later loss)
      if (!wasGood && (!goodHeartMs || nowMs - goodHeartMs > 5000u)) heartGoodSince = nowMs;
      goodHeartMs = nowMs; lostSince = 0;
    } else if (wasGood || lostSince == 0) {
      // lost: only counts as "lost while still" if no movement happened since the last good heart
      if (lostSince == 0 && goodHeartMs) lostSince = goodHeartMs;
    }
    wasGood = good;
    if (good) {
      hrFast = anaNum(hrFast) ? 0.9f * hrFast + 0.1f * s.hr : s.hr;          // ~10 s
      hrSlow = anaNum(hrSlow) ? 0.995f * hrSlow + 0.005f * s.hr : s.hr;      // ~200 s
    }
    if (anaNum(s.rr) && s.sqr >= 30) {
      rrFast = anaNum(rrFast) ? 0.9f * rrFast + 0.1f * s.rr : s.rr;
      rrSlow = anaNum(rrSlow) ? 0.995f * rrSlow + 0.005f * s.rr : s.rr;
    }
    // ---- v1.3 minute record
    const uint32_t idx = (nowMs - t0) / 60000u;
    while (curIdx < idx) { closeMinute(); curIdx++; if (idx - curIdx > ANA_LT_N) curIdx = idx - ANA_LT_N; }
    uint32_t dt = lastMs ? nowMs - lastMs : 1000u;
    if (dt > 3000u) dt = 3000u;
    lastMs = nowMs;
    const bool hrQ = good, rrQ = anaNum(s.rr) && s.sqr >= 40;
    if ((hrQ || rrQ) && !s.mot) acc[0] += dt;
    if (anaFeverNow(s)) acc[1] += dt;
    if (rrQ && s.rr > P.rrHi) acc[2] += dt;
    if (hrQ && s.hr > P.hrHi) acc[3] += dt;
    if (anaIrregNow(s)) acc[4] += dt;
    if ((s.pf & PF_PERIODIC) && rrQ) acc[5] += dt;
    if (anaStressNow(s, P)) acc[6] += dt;
    if (s.ctot >= 0) {
      if (lastCtot >= 0 && s.ctot >= lastCtot) curCough = (uint16_t)(curCough + (s.ctot - lastCtot));
      lastCtot = s.ctot;
    }
    const bool pauseNow = P.apneaSec > 0 && s.apc >= P.apneaSec;
    if (pauseNow && !inPause) curPause++;
    inPause = pauseNow;
  }
  uint32_t minutes() const { return histN; }
  // aggregate of the last W completed minutes (5-minute blocks for coughs)
  AnaLt window(int W) const {
    AnaLt a; memset(&a, 0, sizeof a);
    const uint32_t have = histN < (uint32_t)ANA_LT_N ? histN : (uint32_t)ANA_LT_N;
    const uint32_t n = (uint32_t)W < have ? (uint32_t)W : have;
    a.n = (int)n;
    int blk = 0; long blkKey = -1;
    for (uint32_t k = 0; k < n; k++) {
      const uint32_t mi = histN - 1 - k;              // absolute minute index
      const AnaMin& m = hist[mi % ANA_LT_N];
      if (m.valid >= 30) a.obs++;
      a.coughs += m.coughs; a.pauses += m.pauses;
      if (m.fever >= 30) a.fever++;
      if (m.tachyp >= 30) a.tachyp++;
      if (m.tachy >= 30) a.tachy++;
      if (m.irreg >= 40) a.irreg++;
      if (m.periodic >= 30) a.periodic++;
      if (m.stress >= 45) a.stress++;
      const long key = (long)(mi / 5);
      if (key != blkKey) { if (blk >= 2) a.coughBlocks++; blk = 0; blkKey = key; }
      blk += m.coughs;
    }
    if (blk >= 2) a.coughBlocks++;
    return a;
  }
};

// ---- the written result --------------------------------------------------------------------
struct AnaLine { char lvl; char txt[ANA_TXT]; };   // lvl: 'o' ok, 'i' info, 'w' warning, 'a' alert
struct Verdict {
  uint8_t severity = 0;            // 0 normal, 1 watch, 2 warning, 3 critical
  uint8_t mlClass = 0; float mlScore = 0; float probs[ML_N_CLASS] = {0};
  uint8_t reliability = 0;         // 0..100
  bool agree = true;               // rules and ML point the same way
  char headline[ANA_TXT] = "";
  char condition[ANA_TXT] = "";
  AnaLine ev[ANA_MAX_EV]; int nEv = 0;
  char miss[ANA_MAX_MISS][ANA_TXT]; int nMiss = 0;
  char ruleCode[12] = "ok";        // short code of the winning rule
  int obsMin = 0;                  // v1.3: minutes of calm monitoring so far (long-term rules)
  int news = -1;                   // v1.3: partial NEWS2 score, -1 = not computed
  void clear() {                   // in place: no 2 kB temporary on the task stack
    severity = 0; mlClass = 0; mlScore = 0; for (int k = 0; k < ML_N_CLASS; k++) probs[k] = 0;
    reliability = 0; agree = true; headline[0] = 0; condition[0] = 0; nEv = 0; nMiss = 0;
    ruleCode[0] = 'o'; ruleCode[1] = 'k'; ruleCode[2] = 0; obsMin = 0; news = -1;
  }
  void addEv(char lvl, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
  void addMiss(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
#include <stdarg.h>
inline void Verdict::addEv(char lvl, const char* fmt, ...) {
  if (nEv >= ANA_MAX_EV) return;
  ev[nEv].lvl = lvl;
  va_list ap; va_start(ap, fmt); vsnprintf(ev[nEv].txt, ANA_TXT, fmt, ap); va_end(ap);
  nEv++;
}
inline void Verdict::addMiss(const char* fmt, ...) {
  if (nMiss >= ANA_MAX_MISS) return;
  va_list ap; va_start(ap, fmt); vsnprintf(miss[nMiss], ANA_TXT, fmt, ap); va_end(ap);
  nMiss++;
}

// zone of a value against [lo, hi]: 0 normal, +1/+2/+3 above (mild/moderate/severe), -1/-2/-3 below
static inline int anaZone(float v, float lo, float hi) {
  if (v > hi) return v <= hi * 1.2f ? 1 : v <= hi * 1.5f ? 2 : 3;
  if (v < lo) return v >= lo * 0.83f ? -1 : v >= lo * 0.667f ? -2 : -3;
  return 0;
}
static const char* const ANA_ZN[4] = {"normal", "mildly", "moderately", "severely"};

// NEWS2 (Royal College of Physicians 2017) sub-scores for the parts this monitor can measure
static inline int anaNewsRR(float rr) { return rr <= 8 ? 3 : rr <= 11 ? 1 : rr <= 20 ? 0 : rr <= 24 ? 2 : 3; }
static inline int anaNewsHR(float hr) { return hr <= 40 ? 3 : hr <= 50 ? 1 : hr <= 90 ? 0 : hr <= 110 ? 1 : hr <= 130 ? 2 : 3; }
static inline int anaNewsT(float t) { return t <= 35.0f ? 3 : t <= 36.0f ? 1 : t <= 38.0f ? 0 : t <= 39.0f ? 1 : 2; }

// helper: rule candidates compete by (severity, priority)
struct AnaBest {
  int sev = -1, pri = -1; char code[12] = ""; char head[ANA_TXT] = ""; char cond[ANA_TXT] = "";
  void offer(int s, int p, const char* c, const char* h, const char* d) {
    if (s > sev || (s == sev && p > pri)) { sev = s; pri = p; strlcpy_(code, c, sizeof(code)); strlcpy_(head, h, sizeof(head)); strlcpy_(cond, d, sizeof(cond)); }
  }
  static void strlcpy_(char* d, const char* s, size_t n) { size_t i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }
};

inline void anaEvaluate(const AnaTracker& T, const AnaIn& s, const AgeProfile& P, uint32_t nowMs, Verdict& v) {
  v.clear();
  AnaBest best;
  const bool hrOk = s.pres && anaNum(s.hr), rrOk = anaNum(s.rr);
  char h[ANA_TXT], c[ANA_TXT];
  const uint32_t runMs = T.t0 ? nowMs - T.t0 : 0;
  const AnaLt L60 = T.window(60), L20 = T.window(20), L10 = T.window(10);
  v.obsMin = L60.obs;

  // ---- zone rules (facts: measured values against the normal range for the age) -----------
  if (hrOk && !s.art) {
    int z = anaZone(s.hr, P.hrLo, P.hrHi);
    if (z > 0) {
      snprintf(h, sizeof h, "Heart rate %s above range", ANA_ZN[z]); snprintf(c, sizeof c, "Fast heart rate (%.0f bpm)", s.hr);
      best.offer(z, 40 + z, "tachy", h, c);
      v.addEv(z >= 2 ? 'a' : 'w', "Heart rate %.0f bpm is %s above the age range %.0f-%.0f bpm.", s.hr, ANA_ZN[z], P.hrLo, P.hrHi);
    } else if (z < 0) {
      snprintf(h, sizeof h, "Heart rate %s below range", ANA_ZN[-z]); snprintf(c, sizeof c, "Slow heart rate (%.0f bpm)", s.hr);
      best.offer(-z, 40 - z, "brady", h, c);
      v.addEv(-z >= 2 ? 'a' : 'w', "Heart rate %.0f bpm is %s below the age range %.0f-%.0f bpm.", s.hr, ANA_ZN[-z], P.hrLo, P.hrHi);
    } else v.addEv('o', "Heart rate %.0f bpm is inside the age range %.0f-%.0f bpm.", s.hr, P.hrLo, P.hrHi);
  }
  if (s.art) v.addEv('i', "Strong breathing movement shakes the chest: heart rate paused until breathing calms (not used by any rule meanwhile).");
  if (rrOk) {
    int z = anaZone(s.rr, P.rrLo, P.rrHi);
    if (z > 0) {
      snprintf(h, sizeof h, "Breathing rate %s above range", ANA_ZN[z]); snprintf(c, sizeof c, "Fast breathing (%.1f /min)", s.rr);
      best.offer(z, 50 + z, "tachyp", h, c);
      v.addEv(z >= 2 ? 'a' : 'w', "Breathing rate %.1f /min is %s above the age range %.0f-%.0f /min.", s.rr, ANA_ZN[z], P.rrLo, P.rrHi);
    } else if (z < 0) {
      snprintf(h, sizeof h, "Breathing rate %s below range", ANA_ZN[-z]); snprintf(c, sizeof c, "Slow breathing (%.1f /min)", s.rr);
      best.offer(-z, 50 - z, "bradyp", h, c);
      v.addEv(-z >= 2 ? 'a' : 'w', "Breathing rate %.1f /min is %s below the age range %.0f-%.0f /min.", s.rr, ANA_ZN[-z], P.rrLo, P.rrHi);
    } else v.addEv('o', "Breathing rate %.1f /min is inside the age range %.0f-%.0f /min.", s.rr, P.rrLo, P.rrHi);
  }

  // ---- spectral rules --------------------------------------------------------------------
  if (hrOk && !s.art) {
    float dis = 0;
    if (anaNum(s.hra)) dis = fmaxf(dis, fabsf(s.hra - s.hr));
    if (anaNum(s.hrs)) dis = fmaxf(dis, fabsf(s.hrs - s.hr));
    if (anaNum(s.hrb)) dis = fmaxf(dis, fabsf(s.hrb - s.hr));
    dis = s.hr > 0 ? dis / s.hr : 0;
    const float rel = (anaNum(s.rmssd) && anaNum(s.nn) && s.nn > 200) ? s.rmssd / s.nn : 0;
    if (anaIrregNow(s)) {
      // v1.3: an illness is only suggested when the irregular rhythm lasts (>= 5 of the last 10 calm minutes)
      if (L10.irreg >= ANA_NEED_AF && L10.obs >= ANA_NEED_AF) {
        snprintf(c, sizeof c, "Irregular heart rhythm for %d min: possible atrial fibrillation or frequent extra beats - an ECG is needed", L10.irreg);
        best.offer(2, 62, "irreg", "Irregular heart rhythm (lasting)", c);
        v.addEv('a', "Rhythm irregular for %d of the last %d minutes (spectrum prominence %.1f, estimators differ %.0f %%, RMSSD %.0f %% of interval).",
                L10.irreg, L10.n, s.sp, dis * 100, rel * 100);
      } else {
        best.offer(1, 61, "irreg", "Irregular heartbeat noted", "Irregular heartbeat: the monitor is checking whether it lasts (5 minutes) before suggesting a cause");
        v.addEv('w', "No clear beat-spectrum peak (%.1f), estimators differ by %.0f %%, RMSSD %.0f %% of the interval: irregular beats now.", s.sp, dis * 100, rel * 100);
      }
    } else if (dis > 0.15f) {
      v.addEv('i', "Heart-rate estimators disagree by %.0f %%; treat the value with caution.", dis * 100);
    }
  }
  if (rrOk) {
    const float rdis = (s.rr > 0 && anaNum(s.rrs)) ? fabsf(s.rrs - s.rr) / s.rr : 0;
    if (anaNum(s.pcv) && s.pcv > 0.35f && rdis > 0.25f) {
      best.offer(1, 30, "irrb", "Irregular breathing pattern", "Irregular breathing timing (spectrum and breath counting disagree)");
      v.addEv('w', "Breath timing varies by %.0f %% and the spectral rate differs by %.0f %% from the counted rate.", s.pcv * 100, rdis * 100);
    }
    if (anaNum(s.amp1) && anaNum(s.amp) && s.amp > 0 && s.amp1 < 0.5f * s.amp && s.rr > P.rrHi) {
      best.offer(2, 55, "labour", "Fast, shallow breathing", "Fast and shallow breathing (sign of breathing difficulty)");
      v.addEv('a', "Breathing is fast (%.1f /min) and the last minute is only %.0f %% as deep as the average.", s.rr, 100.0f * s.amp1 / s.amp);
    }
  }
  // coughing is a symptom: shown, but never escalated to an alarm by itself
  if (s.cgh >= 3) {
    snprintf(h, sizeof h, s.cgh >= 6 ? "Frequent coughing" : "Coughing");
    snprintf(c, sizeof c, "Coughing: %d cough bursts in the last 30 s", s.cgh);
    best.offer(1, 45, "cough", h, c);
    v.addEv('w', "%d cough bursts in the last 30 s (explosive chest vibration, not movement).", s.cgh);
  }

  // ---- event rules -----------------------------------------------------------------------
  if (s.apc >= P.apneaSec && P.apneaSec > 0) {
    snprintf(h, sizeof h, "Breathing pause %.0f s", s.apc);
    best.offer(3, 90, "apnea", h, "No breathing detected (breathing pause in progress)");
    v.addEv('a', "No breath detected for %.0f s (alarm threshold %.0f s for this age).", s.apc, P.apneaSec);
  } else if (s.ap5 > 0) {
    int sv = (s.ap5 >= 3 || s.apl >= 2 * P.apneaSec) ? 2 : 1;
    snprintf(h, sizeof h, "%d breathing pause%s in 5 min", s.ap5, s.ap5 > 1 ? "s" : "");
    best.offer(sv, 52, "pauses", h, sv == 2 ? "Repeated breathing pauses: watch the person closely" : "A breathing pause was detected");
    v.addEv(sv == 2 ? 'a' : 'w', "%d pause%s of %.0f s or longer in 5 min (longest %.0f s).", s.ap5, s.ap5 > 1 ? "s" : "", P.apneaSec, s.apl);
  }
  // heart signal lost while the person is still and breathing signal/contact remain -> possible cardiac arrest
  {
    const uint32_t sinceGood = T.goodHeartMs ? nowMs - T.goodHeartMs : 0;
    const bool wasGoodLong = T.goodHeartMs && T.heartGoodSince && (T.goodHeartMs - T.heartGoodSince) >= 30000u;
    const bool still = !s.mot && !s.art && (nowMs - T.lastMotionMs) > 8000u && s.mf < 0.35f;
    if (!hrOk && wasGoodLong && still && sinceGood >= 10000u && sinceGood <= 600000u && T.lastMotionMs < T.goodHeartMs) {
      const bool noBreath = s.apc >= 6 || !anaNum(s.rr);
      snprintf(h, sizeof h, "Heart signal lost for %lu s", (unsigned long)(sinceGood / 1000));
      best.offer(3, noBreath ? 100 : 95, "arrest", h, noBreath ? "Possible cardiac arrest: no heartbeat and no breathing detected" : "Possible cardiac arrest or asystole: heartbeat vibration lost while still");
      v.addEv('a', "A good heart signal (%.0f s long) disappeared %lu s ago without any movement; sensor contact is unchanged%s. Check responsiveness and pulse now.",
              (T.goodHeartMs - T.heartGoodSince) / 1000.0f, (unsigned long)(sinceGood / 1000), noBreath ? " and no breathing is detected" : "");
    }
  }
  if (hrOk && !s.art && s.hr < P.hrLo * 0.5f && s.sqh >= 40) {
    best.offer(3, 92, "brady3", "Very slow heart rate", "Very slow heart rate: possible impending arrest - check the person now");
  }
  // early deterioration trend (v1.3: only after 10 minutes, so the start of a session cannot look like a trend)
  if (anaNum(T.hrFast) && anaNum(T.hrSlow) && anaNum(T.rrFast) && anaNum(T.rrSlow) && runMs > 600000u) {
    const float dh = T.hrFast - T.hrSlow, dr = T.rrFast - T.rrSlow;
    if (dh > 0.12f * T.hrSlow && dr > 3.0f) {
      best.offer(1, 35, "trend", "Early warning: heart and breathing rates rising", "Heart and breathing rates rising together (early sign of deterioration)");
      v.addEv('w', "Heart rate is up %.0f bpm and breathing rate up %.1f /min compared with the last few minutes.", dh, dr);
    }
  }
  if (anaFeverNow(s)) v.addEv('w', "Body temperature %.1f degC (%.1f degF) is raised (sensor calibrated against a thermometer).", s.tbody, s.tbody * 9.0f / 5.0f + 32.0f);
  else if (anaNum(s.tbody) && s.tbody >= 37.8f && !s.tcal) v.addEv('i', "The sensor temperature reads %.1f degC (%.1f degF), but it is not calibrated against a thermometer (Settings): not used as fever.", s.tbody, s.tbody * 9.0f / 5.0f + 32.0f);

  // ---- v1.3 long-term patterns (only after enough calm monitoring) -------------------------
  {
    // chest infection: coughing that keeps coming back (>= 3 separate 5-minute blocks, >= 10 coughs)
    // together with fever or fast breathing for a long part of at least 30 calm minutes
    const bool coughing = L60.coughBlocks >= 3 && L60.coughs >= 10;
    if (coughing && L60.obs >= ANA_NEED_INFECT && (L60.fever >= 10 || L60.tachyp * 2 >= L60.obs)) {
      snprintf(c, sizeof c, "Pattern compatible with a chest infection (for example flu, COVID-19 or pneumonia) - a test and a doctor's review are needed");
      best.offer(2, 60, "resp-inf", "Chest infection pattern", c);
      v.addEv('w', "Over %d calm minutes: %d coughs in %d separate 5-minute periods, %s%s%s.", L60.obs, L60.coughs, L60.coughBlocks,
              L60.fever >= 10 ? "raised temperature" : "", (L60.fever >= 10 && L60.tachyp * 2 >= L60.obs) ? " and " : "",
              L60.tachyp * 2 >= L60.obs ? "fast breathing" : "");
    } else if (coughing) {
      v.addEv('i', "Coughing keeps returning (%d coughs). Infection patterns need %d calm minutes with fever or fast breathing (now %d).",
              L60.coughs, ANA_NEED_INFECT, L60.obs);
    }
    // sleep-apnoea pattern: pauses per hour of calm monitoring (like the apnoea index of a sleep study)
    if (L60.obs >= ANA_NEED_APNOEA && L60.pauses > 0) {
      const float idx = 60.0f * L60.pauses / (float)L60.obs;
      if (idx >= 5.0f) {
        const int sv = idx >= 15.0f ? 2 : 1;
        snprintf(h, sizeof h, "%.0f breathing pauses per hour", idx);
        snprintf(c, sizeof c, "%s", idx >= 30.0f ? "Pattern compatible with severe sleep apnoea - a sleep study and a doctor's review are needed"
                                : idx >= 15.0f ? "Pattern compatible with moderate sleep apnoea - a sleep study is advised"
                                               : "Pattern compatible with mild sleep apnoea - discuss with a doctor");
        best.offer(sv, 53, "osa", h, c);
        v.addEv(sv == 2 ? 'a' : 'w', "%d breathing pauses of %.0f s or longer in %d calm minutes (%.0f per hour).", L60.pauses, P.apneaSec, L60.obs, idx);
      }
    }
    // Cheyne-Stokes-like periodic breathing (waxing / waning with pauses) lasting >= 10 of 20 minutes
    if (L20.periodic >= ANA_NEED_CSR) {
      best.offer(2, 54, "csr", "Periodic breathing (lasting)", "Periodic (Cheyne-Stokes-like) breathing: seen with heart failure, stroke or at high altitude - needs medical review");
      v.addEv('a', "Breathing has waxed and waned with pauses for %d of the last %d minutes.", L20.periodic, L20.n);
    }
    // body-wide stress signs: fast heart AND fast breathing at rest for >= 15 of the last 20 minutes
    if (L20.stress >= 15 && L20.obs >= 15 && T.histN >= (uint32_t)ANA_NEED_SIRS) {
      snprintf(c, sizeof c, "Fast heart and breathing at rest for %d min%s: signs of body-wide stress (e.g. sepsis, dehydration) - see a doctor",
               L20.stress, L20.fever >= 5 ? " with fever" : "");
      best.offer(2, 59, "sirs", "Fast heart and breathing (lasting)", c);
      v.addEv('a', "Heart rate and breathing rate have both been high for %d of the last %d minutes while resting.", L20.stress, L20.n);
    }
  }
  // ---- v1.3 partial NEWS2 (adults): breathing rate, pulse and (calibrated) temperature
  if (P.id >= 6 && hrOk && !s.art && rrOk && runMs >= 300000u) {
    const bool tOk = s.tcal && anaNum(s.tbody);
    const int a = anaNewsRR(s.rr), b = anaNewsHR(s.hr), t = tOk ? anaNewsT(s.tbody) : 0;
    v.news = a + b + t;
    const bool red = a == 3 || b == 3 || t == 3;
    const char* risk = v.news >= 7 ? "high" : (v.news >= 5 || red) ? "medium" : "low";
    v.addEv(v.news >= 5 || red ? 'w' : 'i', "Early warning score NEWS2 (partial: breathing, pulse%s): %d - %s clinical risk.", tOk ? ", temperature" : "", v.news, risk);
    if (v.news >= 7) best.offer(2, 57, "news", "High early-warning score", "High early-warning score (NEWS2 partial >= 7): urgent clinical review advised");
  }
  if (hrOk && !s.art && P.rmssdLow > 0 && anaNum(s.rmssd) && s.rmssd < P.rmssdLow)
    v.addEv('i', "RMSSD %.0f ms is below about %.0f ms (reduced vagal tone; trend indicator only).", s.rmssd, P.rmssdLow);

  // ---- ML --------------------------------------------------------------------------------
  float x[ML_N_IN];
  anaFeatures(s, P, x);
  anaMl(x, v.probs);
  int top = 0;
  for (int k = 1; k < ML_N_CLASS; k++) if (v.probs[k] > v.probs[top]) top = k;
  v.mlClass = (uint8_t)top; v.mlScore = v.probs[top];

  // ---- fusion ----------------------------------------------------------------------------
  int sev = best.sev < 0 ? 0 : best.sev;
  if (best.sev < 0) {
    if (hrOk || rrOk) {
      if (top != 0 && v.mlScore >= 0.80f && top != 6) {
        sev = 1;
        snprintf(h, sizeof h, "Pattern check: %s", ANA_CLASS[top]);
        snprintf(c, sizeof c, "Model suspects %s (rules found no zone breach)", ANA_CLASS[top]);
        best.offer(1, 10, "ml", h, c);
      } else {
        best.offer(0, 0, "ok", "No abnormality detected", "Heart and breathing pattern within the expected range");
      }
    } else {
      best.offer(0, 0, "none", "Waiting for a usable signal", "No heart or breathing signal yet");
    }
  }
  v.severity = (uint8_t)(sev > 3 ? 3 : sev);
  AnaBest::strlcpy_(v.headline, best.head, ANA_TXT);
  AnaBest::strlcpy_(v.condition, best.cond, ANA_TXT);
  AnaBest::strlcpy_(v.ruleCode, best.code, sizeof(v.ruleCode));
  // agreement: rule category vs ML class
  {
    int ruleCls = 0;
    const char* rc = best.code;
    if (!strcmp(rc, "tachy")) ruleCls = 1; else if (!strcmp(rc, "brady") || !strcmp(rc, "brady3")) ruleCls = 2;
    else if (!strcmp(rc, "irreg")) ruleCls = 3; else if (!strcmp(rc, "tachyp") || !strcmp(rc, "labour")) ruleCls = 4;
    else if (!strcmp(rc, "apnea") || !strcmp(rc, "pauses") || !strcmp(rc, "bradyp") || !strcmp(rc, "osa") || !strcmp(rc, "csr")) ruleCls = 5;
    else if (!strcmp(rc, "cough") || !strcmp(rc, "resp-inf")) ruleCls = 6;
    else if (!strcmp(rc, "arrest") || !strcmp(rc, "sirs") || !strcmp(rc, "news") || !strcmp(rc, "trend")) ruleCls = -1;   // no ML class for these
    v.agree = (ruleCls < 0) || (ruleCls == top) || (sev == 0 && top == 0) || v.mlScore < 0.6f;
    if (sev >= 1 && ruleCls >= 0 && ruleCls != top && v.mlScore >= 0.6f)
      v.addEv('i', "The learned model leans towards \"%s\" (%.0f %%) while the rules found \"%s\".", ANA_CLASS[top], v.mlScore * 100, best.cond);
    else if (sev == 0 && top != 0 && v.mlScore >= 0.6f)
      v.addEv('i', "The learned model leans towards \"%s\" (%.0f %%), below the alert level.", ANA_CLASS[top], v.mlScore * 100);
  }

  // ---- reliability index and missing inputs ---------------------------------------------
  float rel = 0;
  {
    float q = (hrOk && rrOk) ? 0.5f * (s.sqh + s.sqr) : hrOk ? 0.8f * s.sqh : rrOk ? 0.8f * s.sqr : 0.0f;
    q *= 1.0f - 0.7f * anaClip(s.mf, 0, 1);
    if (hrOk) {
      float dis = 0;
      if (anaNum(s.hra)) dis = fmaxf(dis, fabsf(s.hra - s.hr));
      if (anaNum(s.hrs)) dis = fmaxf(dis, fabsf(s.hrs - s.hr));
      if (s.hr > 0) q *= 1.0f - anaClip(dis / s.hr, 0, 0.3f);
    }
    if (s.mot) q *= 0.5f;
    if (s.art) q *= 0.6f;
    rel = anaClip(q, 0, 100);
  }
  v.reliability = (uint8_t)lroundf(rel);
  if (!hrOk) v.addMiss("No reliable heart signal: heart rate, rhythm and spectral rules were skipped.");
  else if (!(anaNum(s.rmssd))) v.addMiss("Heart-rate variability needs about 1 minute of still recording (20+ beats).");
  if (!rrOk) v.addMiss("No reliable breathing signal: breathing rules were skipped.");
  else if (s.win < 150) v.addMiss("Breathing window is only %.0f s of the 5 min used; the rate may still shift.", s.win);
  if (L60.obs < ANA_NEED_INFECT) v.addMiss("Long-term pattern checks (infection, sleep apnoea, lasting irregular rhythm) need %d calm minutes: %d so far.", ANA_NEED_INFECT, L60.obs);
  else if (!anaNum(s.tbody) || !s.tcal) v.addMiss("Body temperature is not calibrated (Settings): fever is not used.");
  if (s.mf > 0.2f) v.addMiss("Movement covered %.0f %% of the last minute; those parts were excluded.", s.mf * 100);
  if (v.nEv == 0) v.addEv('i', "Not enough valid data to list evidence yet.");
}

// ---- JSON ("v" message) -----------------------------------------------------------------------
template <class J>
void anaWriteJson(J& j, const Verdict& v) {
  j.raw("{\"t\":\"v\"");
  j.ki("sev", v.severity); j.ks("code", v.ruleCode); j.ks("head", v.headline); j.ks("cond", v.condition);
  j.kf("score", v.mlScore, 3); j.ki("mlc", v.mlClass); j.ks("mln", ANA_CLASS[v.mlClass]); j.ki("rel", v.reliability); j.ki("agree", v.agree ? 1 : 0);
  j.ki("obs", v.obsMin); j.ki("need", ANA_NEED_INFECT); j.ki("news", v.news);
  j.key("p"); j.raw("["); for (int k = 0; k < ML_N_CLASS; k++) { if (k) j.raw(","); j.num(v.probs[k], 3); } j.raw("]");
  j.key("ev"); j.raw("[");
  for (int i = 0; i < v.nEv; i++) { if (i) j.raw(","); j.raw("["); char l[2] = {v.ev[i].lvl, 0}; j.raw("\""); j.raw(l); j.raw("\",\"");
    for (const char* p = v.ev[i].txt; *p; p++) { if (*p == '"' || *p == '\\') j.raw("\\"); char ch[2] = {*p, 0}; if ((unsigned char)*p >= 0x20) j.raw(ch); }
    j.raw("\"]"); }
  j.raw("]");
  j.key("miss"); j.raw("[");
  for (int i = 0; i < v.nMiss; i++) { if (i) j.raw(","); j.raw("\"");
    for (const char* p = v.miss[i]; *p; p++) { if (*p == '"' || *p == '\\') j.raw("\\"); char ch[2] = {*p, 0}; if ((unsigned char)*p >= 0x20) j.raw(ch); }
    j.raw("\""); }
  j.raw("]}");
}
