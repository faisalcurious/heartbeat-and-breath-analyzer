// =====================================================================
//  AgeNorms.h  -  age dependent reference values (resting, awake)
//
//  Normal ranges follow the AHA / PALS (2020) tables for children and
//  the AHA adult ranges. "Search" ranges are wider physiological limits
//  used to tune the detectors (filter bands, refractory periods, valid
//  interval limits). The web page carries the same table for its text
//  analysis - keep both in sync if you edit values.
// =====================================================================
#pragma once

struct AgeProfile {
  uint8_t id;             // group index
  const char* name;       // group label
  float hrLo, hrHi;       // normal resting heart rate (bpm)
  float rrLo, rrHi;       // normal resting breathing rate (breaths/min)
  float hrSearchLo, hrSearchHi;  // detector limits (bpm)
  float rrSearchLo, rrSearchHi;  // detector limits (breaths/min)
  float apneaSec;         // pause length flagged as apnea / breathing pause
  float rmssdLow;         // indicative lower RMSSD (ms), 0 = no adult norm
};

static const AgeProfile AGE_TABLE[] = {
  //id name                      hrLo hrHi  rrLo rrHi  hrS-  hrS+  rrS- rrS+  apnea rmssd
  {0, "Newborn (0-1 month)",     100, 205,  30,  60,   70,  230,  15,  90,   20,   0},
  {1, "Infant (1-12 months)",    100, 180,  30,  53,   70,  220,  15,  80,   20,   0},
  {2, "Toddler (1-2 years)",      98, 140,  22,  37,   60,  200,  10,  60,   15,   0},
  {3, "Preschool (3-5 years)",    80, 120,  20,  28,   55,  190,   8,  50,   15,   0},
  {4, "School age (6-11 years)",  75, 118,  18,  25,   45,  180,   6,  45,   15,   0},
  {5, "Adolescent (12-17 years)", 60, 100,  12,  20,   30,  230,   5,  60,   10,  20},
  {6, "Adult (18-39 years)",      60, 100,  12,  20,   30,  230,   4,  60,   10,  20},
  {7, "Adult (40-64 years)",      60, 100,  12,  20,   30,  220,   4,  60,   10,  15},
  {8, "Older adult (65+ years)",  60, 100,  12,  20,   30,  210,   4,  60,   10,  10},
};

static inline const AgeProfile& ageProfileFor(float years) {
  int i;
  if (years < 1.0f / 12.0f) i = 0;
  else if (years < 1.0f) i = 1;
  else if (years < 3.0f) i = 2;
  else if (years < 6.0f) i = 3;
  else if (years < 12.0f) i = 4;
  else if (years < 18.0f) i = 5;
  else if (years < 40.0f) i = 6;
  else if (years < 65.0f) i = 7;
  else i = 8;
  return AGE_TABLE[i];
}
