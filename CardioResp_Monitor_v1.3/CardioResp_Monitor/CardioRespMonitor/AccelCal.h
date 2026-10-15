// =====================================================================
//  AccelCal.h  -  one-time accelerometer accuracy calibration
//
//  Cheap MPU6050 modules often have offsets of 0.1-0.3 g and gain errors
//  of a few percent (the start-up gravity check then reads e.g. 1.2 g).
//  The user places the bare sensor still in 6 (or more) orientations - each
//  face up once. From the mean readings we fit, by Levenberg-Marquardt,
//        a_true = (a_raw - offset) * gain      (per axis)
//  so that every captured vector has magnitude exactly 1 g. The result is
//  stored in flash and applied to every sample before any processing.
//  Portable (no Arduino dependencies): unit-tested on a PC.
// =====================================================================
#pragma once
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define ACC_CAL_MAGIC 0xACC1CA1Bu

struct AccelCorrection {
  uint32_t magic = 0;
  float offset[3] = {0, 0, 0};   // g
  float gain[3] = {1, 1, 1};
  float rmsMg = 0;               // fit residual (mg)
  uint8_t points = 0;
  uint8_t full = 0;              // 1 = offsets + gains, 0 = offsets only
  bool valid() const { return magic == ACC_CAL_MAGIC; }
  inline void apply(float& x, float& y, float& z) const {
    if (magic != ACC_CAL_MAGIC) return;
    x = (x - offset[0]) * gain[0];
    y = (y - offset[1]) * gain[1];
    z = (z - offset[2]) * gain[2];
  }
};

struct AccelCalSession {
  static const int MAXP = 12;
  float pts[MAXP][3];
  int n = 0;
  volatile bool capturing = false;
  int got = 0;
  static const int NEED = 400;          // 2 s at 200 Hz
  double s[6], s2[6];
  char msg[112] = "Place the bare sensor still, one face up, then press Capture.";

  void clear() { n = 0; capturing = false; got = 0; snprintf(msg, sizeof(msg), "Cleared. Place the sensor still, one face up, then press Capture."); }
  void startCapture() {
    if (n >= MAXP) { snprintf(msg, sizeof(msg), "Maximum of %d positions reached - press Save.", MAXP); return; }
    for (int i = 0; i < 6; i++) { s[i] = 0; s2[i] = 0; }
    got = 0;
    capturing = true;
    snprintf(msg, sizeof(msg), "Capturing position %d - do not touch...", n + 1);
  }
  // feed raw samples (g, dps); returns true when a capture just finished
  bool addSample(const float v[6]) {
    if (!capturing) return false;
    for (int i = 0; i < 6; i++) { s[i] += v[i]; s2[i] += (double)v[i] * v[i]; }
    if (++got < NEED) return false;
    capturing = false;
    float m[3], sa = 0, sg = 0;
    for (int i = 0; i < 6; i++) {
      double mean = s[i] / got, var = s2[i] / got - mean * mean;
      float sd = var > 0 ? (float)sqrt(var) : 0.0f;
      if (i < 3) { m[i] = (float)mean; if (sd > sa) sa = sd; } else if (sd > sg) sg = sd;
    }
    if (sa > 0.02f || sg > 2.0f) { snprintf(msg, sizeof(msg), "Moved during capture - rest the sensor on a table and try again."); return true; }
    float mm = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    if (mm < 0.5f || mm > 1.6f) { snprintf(msg, sizeof(msg), "Reading of %.2f g is not plausible - check the sensor.", mm); return true; }
    for (int k = 0; k < n; k++) {
      float d = 0, nk = 0;
      for (int i = 0; i < 3; i++) { d += pts[k][i] * m[i]; nk += pts[k][i] * pts[k][i]; }
      float c = d / (sqrtf(nk) * mm);
      if (c > 0.906f) { snprintf(msg, sizeof(msg), "Same position as capture %d - turn the sensor onto another face.", k + 1); return true; }  // < 25 deg apart
    }
    for (int i = 0; i < 3; i++) pts[n][i] = m[i];
    n++;
    char miss[48];
    int cov = coverage(miss, sizeof(miss));
    if (cov == 0x3F) snprintf(msg, sizeof(msg), "Position %d saved (%.3f g). All 6 faces done - press Save.", n, mm);
    else snprintf(msg, sizeof(msg), "Position %d saved (%.3f g). Still needed: %s", n, mm, miss);
    return true;
  }
  // bit mask of faces pointing up: +X -X +Y -Y +Z -Z  (component > 0.7 g)
  int coverage(char* missing, int len) const {
    int m = 0;
    for (int k = 0; k < n; k++) for (int i = 0; i < 3; i++) {
      float nk = sqrtf(pts[k][0] * pts[k][0] + pts[k][1] * pts[k][1] + pts[k][2] * pts[k][2]);
      if (pts[k][i] / nk > 0.7f) m |= 1 << (2 * i);
      if (pts[k][i] / nk < -0.7f) m |= 1 << (2 * i + 1);
    }
    if (missing && len > 0) {
      static const char* names[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
      missing[0] = 0;
      for (int b = 0; b < 6; b++) if (!(m & (1 << b))) {
        size_t l = strlen(missing);
        snprintf(missing + l, len - l, "%s%s up", l ? ", " : "", names[b]);
      }
    }
    return m;
  }

  // Levenberg-Marquardt fit. Full model (offsets + gains) needs all six
  // faces; with at least 4 well-spread positions only offsets are fitted.
  bool solve(AccelCorrection& out, char* err, int len) const {
    char miss[48];
    const int cov = coverage(miss, sizeof(miss));
    const bool full = (cov == 0x3F) && n >= 6;
    if (!full && n < 4) { snprintf(err, len, "Need at least 4 positions (6 faces for best accuracy). Missing: %s", miss); return false; }
    const int np = full ? 6 : 3;
    double p[6] = {0, 0, 0, 1, 1, 1};
    double lambda = 1e-3;
    auto cost = [&](const double* q) {
      double c = 0;
      for (int k = 0; k < n; k++) {
        double u0 = q[3] * (pts[k][0] - q[0]), u1 = q[4] * (pts[k][1] - q[1]), u2 = q[5] * (pts[k][2] - q[2]);
        double r = sqrt(u0 * u0 + u1 * u1 + u2 * u2) - 1.0;
        c += r * r;
      }
      return c;
    };
    double c0 = cost(p);
    for (int it = 0; it < 100; it++) {
      double A[6][6] = {{0}}, g[6] = {0};
      for (int k = 0; k < n; k++) {
        double d[3], u[3];
        for (int i = 0; i < 3; i++) { d[i] = pts[k][i] - p[i]; u[i] = p[3 + i] * d[i]; }
        double L = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (L < 1e-9) continue;
        double r = L - 1.0, J[6];
        for (int i = 0; i < 3; i++) { J[i] = -p[3 + i] * u[i] / L; J[3 + i] = d[i] * u[i] / L; }
        for (int a = 0; a < np; a++) { g[a] += J[a] * r; for (int b = 0; b < np; b++) A[a][b] += J[a] * J[b]; }
      }
      // solve (A + lambda diag(A)) dp = -g  by Gaussian elimination
      double M[6][7];
      for (int a = 0; a < np; a++) { for (int b = 0; b < np; b++) M[a][b] = A[a][b] + (a == b ? lambda * (A[a][a] + 1e-9) : 0.0); M[a][np] = -g[a]; }
      bool sing = false;
      for (int c = 0; c < np && !sing; c++) {
        int piv = c;
        for (int r = c + 1; r < np; r++) if (fabs(M[r][c]) > fabs(M[piv][c])) piv = r;
        if (fabs(M[piv][c]) < 1e-15) { sing = true; break; }
        if (piv != c) for (int k = 0; k <= np; k++) { double t = M[c][k]; M[c][k] = M[piv][k]; M[piv][k] = t; }
        for (int r = 0; r < np; r++) if (r != c) {
          double f = M[r][c] / M[c][c];
          for (int k = c; k <= np; k++) M[r][k] -= f * M[c][k];
        }
      }
      if (sing) { snprintf(err, len, "Positions too similar - use clearly different faces."); return false; }
      double q[6];
      for (int a = 0; a < 6; a++) q[a] = p[a];
      double step = 0;
      for (int a = 0; a < np; a++) { double dp = M[a][np] / M[a][a]; q[a] += dp; step += dp * dp; }
      double c1 = cost(q);
      if (c1 < c0) { for (int a = 0; a < 6; a++) p[a] = q[a]; c0 = c1; lambda *= 0.3; if (step < 1e-14) break; }
      else { lambda *= 10; if (lambda > 1e6) break; }
    }
    const double rms = sqrt(c0 / n) * 1000.0;
    for (int i = 0; i < 3; i++) {
      if (fabs(p[i]) > 0.5 || p[3 + i] < 0.8 || p[3 + i] > 1.2) { snprintf(err, len, "Result not plausible (offset %.2f g, gain %.2f) - recapture.", p[i], p[3 + i]); return false; }
    }
    if (rms > 25.0) { snprintf(err, len, "Fit error %.0f mg is too high - the sensor probably moved. Clear and recapture.", rms); return false; }
    out.magic = ACC_CAL_MAGIC;
    for (int i = 0; i < 3; i++) { out.offset[i] = (float)p[i]; out.gain[i] = (float)p[3 + i]; }
    out.rmsMg = (float)rms;
    out.points = (uint8_t)n;
    out.full = full ? 1 : 0;
    snprintf(err, len, "Saved: offsets %.3f %.3f %.3f g, gains %.3f %.3f %.3f, fit error %.1f mg%s", p[0], p[1], p[2], p[3], p[4], p[5], rms, full ? "" : " (offsets only)");
    return true;
  }
};
