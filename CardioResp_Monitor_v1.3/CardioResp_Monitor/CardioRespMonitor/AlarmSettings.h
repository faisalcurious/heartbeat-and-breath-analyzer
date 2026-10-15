// =====================================================================
//  AlarmSettings.h  -  v1.3 alarm limits (set points) and time windows, editable by a doctor or nurse
//
//  * Every alarm has a warning and an urgent set point and a delay (time window): the condition must last
//    that long before the alarm sounds. Defaults follow the patient's age group (the built-in v1.3 rules).
//  * The limits are kept on the monitor (all phones use the same ones). Everybody can see them; changing them
//    needs the doctor / nurse PIN. The PIN is stored only as a salted SHA-256 hash.
//  * Pure C++ (no Arduino): also compiled on the PC by host_test/alarm_settings_test.cpp.
// =====================================================================
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "MeshAuth.h"            // meshSha256 (mbedtls on the controller, OpenSSL on the PC)

struct AlarmLimits {
  float hrLoU, hrLo, hrHi, hrHiU;          // heart rate (bpm): urgent below, warning below, warning above, urgent above
  float rrLoU, rrLo, rrHi, rrHiU;          // breathing rate (/min, counted over the last minute): same order
  float apnea, apneaU;                     // breathing pause (s): warning after, urgent after
  float tLo, tHi, tHiU;                    // body temperature (degC, only after calibration): warning below, notice above, warning above
  float hrDly, rrDly, tDly, nosigDly;      // delays (s) before the heart, breathing, temperature and "no signal" alarms sound
};

struct AlarmCfg {
  uint8_t version = 1;
  uint8_t autoAge = 1;                     // 1 = use the age-based defaults (follow the age when it changes)
  uint8_t pad[2] = {0, 0};
  AlarmLimits lim{};                       // custom limits (used when autoAge == 0)
  uint32_t at = 0;                         // when they were last changed (epoch seconds from the phone)
  uint32_t seq = 0;                        // change counter (pages reload the limits when it changes)
  char by[24] = "";                        // who changed them
};

struct AlarmLogEntry { uint32_t t; char by[24]; char note[120]; };

static inline float alRound(float v, float step) { return roundf(v / step) * step; }

// defaults from the age group's normal resting ranges = the v1.3 built-in alarm rules
// (warning beyond 20 % above / 17 % below the range, urgent beyond 50 % above / 33 % below)
inline AlarmLimits alarmDefaults(float hrLo, float hrHi, float rrLo, float rrHi, float apneaSec) {
  AlarmLimits L;
  L.hrLo = alRound(0.83f * hrLo, 1);  L.hrHi = alRound(1.2f * hrHi, 1);
  L.hrLoU = alRound(0.667f * hrLo, 1); L.hrHiU = alRound(1.5f * hrHi, 1);
  L.rrLo = alRound(0.83f * rrLo, 1);  L.rrHi = alRound(1.2f * rrHi, 1);
  L.rrLoU = alRound(0.667f * rrLo, 1); L.rrHiU = alRound(1.5f * rrHi, 1);
  L.apnea = apneaSec; L.apneaU = apneaSec > 20 ? apneaSec : 20;
  L.tLo = 34.5f; L.tHi = 38.0f; L.tHiU = 39.5f;
  L.hrDly = 10; L.rrDly = 10; L.tDly = 20; L.nosigDly = 30;
  return L;
}

// checks one set of limits; on failure 'err' says what to fix in plain words
inline bool alarmValidate(const AlarmLimits& L, char* err, size_t n) {
  struct R { const char* name; float v, lo, hi; };
  const R r[] = {
    {"Heart rate urgent-low", L.hrLoU, 15, 200}, {"Heart rate low", L.hrLo, 20, 250}, {"Heart rate high", L.hrHi, 40, 300}, {"Heart rate urgent-high", L.hrHiU, 40, 320},
    {"Breathing urgent-low", L.rrLoU, 2, 60}, {"Breathing low", L.rrLo, 3, 80}, {"Breathing high", L.rrHi, 5, 120}, {"Breathing urgent-high", L.rrHiU, 5, 150},
    {"Breathing pause", L.apnea, 5, 60}, {"Breathing pause urgent", L.apneaU, 5, 120},
    {"Temperature low", L.tLo, 30, 37}, {"Temperature high", L.tHi, 36.5f, 42}, {"Temperature urgent-high", L.tHiU, 36.5f, 43},
    {"Heart rate delay", L.hrDly, 0, 120}, {"Breathing delay", L.rrDly, 0, 120}, {"Temperature delay", L.tDly, 0, 600}, {"No-signal delay", L.nosigDly, 10, 300}};
  for (const R& x : r) {
    if (!(x.v >= x.lo && x.v <= x.hi)) { snprintf(err, n, "%s must be between %g and %g", x.name, (double)x.lo, (double)x.hi); return false; }
  }
  if (!(L.hrLoU <= L.hrLo && L.hrLo < L.hrHi && L.hrHi <= L.hrHiU)) { snprintf(err, n, "Heart rate: urgent-low <= low < high <= urgent-high"); return false; }
  if (!(L.rrLoU <= L.rrLo && L.rrLo < L.rrHi && L.rrHi <= L.rrHiU)) { snprintf(err, n, "Breathing: urgent-low <= low < high <= urgent-high"); return false; }
  if (!(L.apnea <= L.apneaU)) { snprintf(err, n, "Breathing pause: the urgent time must not be shorter than the warning time"); return false; }
  if (!(L.tLo < L.tHi && L.tHi <= L.tHiU)) { snprintf(err, n, "Temperature: low < high <= urgent-high"); return false; }
  return true;
}

// ---- doctor / nurse PIN (4 to 8 digits), stored as hex SHA-256("CardioResp-staff/v1|" + device id + "|" + PIN)
inline bool alarmPinValid(const char* p) {
  const size_t n = p ? strlen(p) : 0;
  if (n < 4 || n > 8) return false;
  for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return false;
  return true;
}
inline void alarmPinHash(const char* devId, const char* pin, char out[65]) {
  char buf[80];
  const int n = snprintf(buf, sizeof buf, "CardioResp-staff/v1|%s|%s", devId ? devId : "", pin ? pin : "");
  uint8_t h[32];
  meshSha256((const uint8_t*)buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1), h);
  static const char* hx = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { out[2 * i] = hx[h[i] >> 4]; out[2 * i + 1] = hx[h[i] & 15]; }
  out[64] = 0;
}
inline bool alarmHashEq(const char* a, const char* b) {     // constant time
  uint8_t d = 0;
  for (int i = 0; i < 64; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0 && a[64] == 0 && b[64] == 0;
}

// ---- wrong-PIN lock-out: 5 wrong PINs in a row -> locked for 60 s, doubling up to 15 minutes
struct PinGuard {
  int wrong = 0;
  uint32_t lockUntil = 0, lockMs = 60000;
  uint32_t lockedFor(uint32_t now) const { return (lockUntil && (int32_t)(lockUntil - now) > 0) ? (lockUntil - now + 999) / 1000 : 0; }
  void fail(uint32_t now) {
    if (++wrong >= 5) { lockUntil = now + lockMs; if (lockMs < 900000u) lockMs *= 2; wrong = 0; }
  }
  void ok() { wrong = 0; lockMs = 60000; lockUntil = 0; }
};
