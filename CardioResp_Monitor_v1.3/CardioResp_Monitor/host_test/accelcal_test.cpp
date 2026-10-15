// Unit test for AccelCal.h: synthetic clone sensor, 6 faces + tilted extras
#include "AccelCal.h"
#include <random>
#include <cstdio>
int main() {
  std::mt19937 rng(7); std::normal_distribution<float> N(0, 1);
  const float off[3] = {0.031f, -0.042f, 0.205f}, gn[3] = {1.021f, 0.978f, 1.046f};   // raw = true/gain + off
  auto capture = [&](AccelCalSession& S, float gx, float gy, float gz, float moveG) {
    float nrm = sqrtf(gx*gx+gy*gy+gz*gz); gx/=nrm; gy/=nrm; gz/=nrm;
    S.startCapture();
    for (int i = 0; i < 400; i++) {
      float t[3] = {gx, gy, gz}, v[6];
      for (int k = 0; k < 3; k++) v[k] = t[k] / gn[k] + off[k] + 0.003f * N(rng) + moveG * sinf(i * 0.3f);
      for (int k = 3; k < 6; k++) v[k] = 0.05f * N(rng);
      S.addSample(v);
    }
    printf("  %s\n", S.msg);
  };
  int fails = 0;
  { // full 6-face calibration + one duplicate + one moved capture
    AccelCalSession S; S.clear();
    capture(S, 0, 0, 1, 0); capture(S, 0, 0, 1, 0); capture(S, 0, 0, -1, 0.05f);
    capture(S, 0, 0, -1, 0); capture(S, 1, 0, 0, 0); capture(S, -1, 0, 0, 0); capture(S, 0, 1, 0, 0); capture(S, 0, -1, 0.1f, 0);
    AccelCorrection C; char msg[160];
    bool ok = S.solve(C, msg, sizeof msg);
    printf("full: %s -> %s\n", ok ? "OK" : "FAIL", msg);
    for (int k = 0; k < 3; k++) if (fabsf(C.offset[k] - off[k]) > 0.003f || fabsf(C.gain[k] - gn[k]) > 0.005f) fails++;
    // check corrected magnitude in a new random orientation
    float x = 0.3f/gn[0]+off[0], y = -0.5f/gn[1]+off[1], z = 0.812404f/gn[2]+off[2];
    C.apply(x, y, z);
    printf("  corrected |g| at a new angle: %.4f\n", sqrtf(x*x+y*y+z*z));
    if (fabsf(sqrtf(x*x+y*y+z*z) - 1) > 0.005f) fails++;
  }
  { // offsets only with 4 positions
    AccelCalSession S; S.clear();
    capture(S, 0, 0, 1, 0); capture(S, 1, 0, 0.2f, 0); capture(S, 0, 1, 0.2f, 0); capture(S, -0.6f, -0.6f, -0.5f, 0);
    AccelCorrection C; char msg[160];
    bool ok = S.solve(C, msg, sizeof msg);
    printf("partial: %s -> %s\n", ok ? "OK" : "FAIL", msg);
    if (!ok) fails++;
  }
  { // too few
    AccelCalSession S; S.clear(); capture(S, 0, 0, 1, 0); capture(S, 1, 0, 0, 0);
    AccelCorrection C; char msg[160];
    bool ok = S.solve(C, msg, sizeof msg);
    printf("too few: %s -> %s\n", ok ? "OK (wrong)" : "rejected", msg);
    if (ok) fails++;
  }
  printf(fails ? "ACCELCAL TEST FAILED (%d)\n" : "ACCELCAL TEST PASSED\n", fails);
  return fails;
}
