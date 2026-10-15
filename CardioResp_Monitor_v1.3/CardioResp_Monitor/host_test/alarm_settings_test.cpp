// =====================================================================
//  alarm_settings_test.cpp - v1.3 alarm limits, validation, doctor / nurse PIN and lock-out (AlarmSettings.h)
//  build: g++ -O2 -std=c++17 -DMESH_AUTH_OPENSSL -ICardioRespMonitor host_test/alarm_settings_test.cpp -lcrypto -o alarm_settings_test
// =====================================================================
#include <cstdio>
#include "AlarmSettings.h"
#include "AgeNorms.h"

static int fails = 0;
static void check(const char* what, bool ok) { printf("  %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

int main() {
  printf("age-based defaults (same as the built-in v1.3 alarm rules)\n");
  const AgeProfile& A = ageProfileFor(70), &I = ageProfileFor(0.5f), &C = ageProfileFor(4);
  AlarmLimits a = alarmDefaults(A.hrLo, A.hrHi, A.rrLo, A.rrHi, A.apneaSec);
  printf("    adult: HR %g/%g .. %g/%g, RR %g/%g .. %g/%g, pause %g/%g s, temp <%g >%g >%g, delays %g/%g/%g/%g s\n",
         a.hrLoU, a.hrLo, a.hrHi, a.hrHiU, a.rrLoU, a.rrLo, a.rrHi, a.rrHiU, a.apnea, a.apneaU, a.tLo, a.tHi, a.tHiU, a.hrDly, a.rrDly, a.tDly, a.nosigDly);
  check("adult heart rate 40 / 50 .. 120 / 150", a.hrLoU == 40 && a.hrLo == 50 && a.hrHi == 120 && a.hrHiU == 150);
  check("adult breathing 8 / 10 .. 24 / 30", a.rrLoU == 8 && a.rrLo == 10 && a.rrHi == 24 && a.rrHiU == 30);
  check("adult pause 10 s, urgent 20 s", a.apnea == 10 && a.apneaU == 20);
  AlarmLimits in = alarmDefaults(I.hrLo, I.hrHi, I.rrLo, I.rrHi, I.apneaSec), ch = alarmDefaults(C.hrLo, C.hrHi, C.rrLo, C.rrHi, C.apneaSec);
  printf("    infant: HR %g..%g RR %g..%g pause %g s;  child 4 y: HR %g..%g RR %g..%g pause %g s\n", in.hrLo, in.hrHi, in.rrLo, in.rrHi, in.apnea, ch.hrLo, ch.hrHi, ch.rrLo, ch.rrHi, ch.apnea);
  check("infant and child defaults follow their age ranges", in.hrHi == 216 && in.apnea == 20 && ch.rrHi > a.rrHi && ch.apnea == 15);
  char err[96];
  check("all defaults pass validation", alarmValidate(a, err, sizeof err) && alarmValidate(in, err, sizeof err) && alarmValidate(ch, err, sizeof err));

  printf("validation of edited limits\n");
  AlarmLimits b = a; b.hrHi = 45;                        // high below low
  check("high below low rejected", !alarmValidate(b, err, sizeof err)); printf("    -> %s\n", err);
  b = a; b.hrHiU = 110;                                  // urgent below warning
  check("urgent-high below high rejected", !alarmValidate(b, err, sizeof err)); printf("    -> %s\n", err);
  b = a; b.apneaU = 8;
  check("urgent pause shorter than warning rejected", !alarmValidate(b, err, sizeof err));
  b = a; b.hrDly = NAN;
  check("not-a-number rejected", !alarmValidate(b, err, sizeof err)); printf("    -> %s\n", err);
  b = a; b.rrHi = 500;
  check("absurd value rejected", !alarmValidate(b, err, sizeof err)); printf("    -> %s\n", err);
  b = a; b.hrHi = 110; b.hrDly = 5; b.apnea = 15; b.apneaU = 25; b.tHi = 37.8f;
  check("sensible custom limits accepted", alarmValidate(b, err, sizeof err));

  printf("doctor / nurse PIN\n");
  check("PIN format: 4-8 digits", alarmPinValid("1234") && alarmPinValid("12345678") && !alarmPinValid("123") && !alarmPinValid("12a4") && !alarmPinValid(""));
  char h1[65], h2[65], h3[65];
  alarmPinHash("A1B2C3", "4821", h1); alarmPinHash("A1B2C3", "4821", h2); alarmPinHash("D4E5F6", "4821", h3);
  printf("    hash %s\n", h1);
  check("same PIN, same monitor: equal", alarmHashEq(h1, h2));
  check("same PIN, other monitor: different (salted)", !alarmHashEq(h1, h3));
  alarmPinHash("A1B2C3", "4822", h2);
  check("wrong PIN: different", !alarmHashEq(h1, h2));

  printf("wrong-PIN lock-out\n");
  PinGuard g; uint32_t t = 1000;
  for (int i = 0; i < 4; i++) g.fail(t);
  check("4 wrong PINs: not locked", g.lockedFor(t) == 0);
  g.fail(t);
  check("5th wrong PIN: locked 60 s", g.lockedFor(t) == 60);
  check("unlocked after 60 s", g.lockedFor(t + 60001) == 0);
  for (int i = 0; i < 5; i++) g.fail(t + 70000);
  check("next lock-out doubles (120 s)", g.lockedFor(t + 70000) == 120);
  g.ok();
  check("a correct PIN resets the lock-out", g.lockedFor(t + 70000) == 0 && g.wrong == 0);
  printf("%s (%d failed)\n", fails ? "SOME ALARM-SETTINGS CHECKS FAILED" : "ALL ALARM-SETTINGS CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
