// =====================================================================
//  DspCore.h  -  portable DSP building blocks (no Arduino dependency)
//  Used by VitalsEngine. Everything here is float-based and allocation
//  free so it runs deterministically on the ESP32-C3 (RISC-V, no FPU)
//  and can be unit-tested on a PC.
// =====================================================================
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSP_DEG2RAD 0.017453292519943295f
#define DSP_RAD2DEG 57.29577951308232f

static inline float dspClamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float dspSq(float x) { return x * x; }

// ---------------------------------------------------------------------
// Biquad (Direct Form II transposed). Coefficients designed in double
// precision (RBJ cookbook) and stored as float.
// ---------------------------------------------------------------------
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  inline float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }

  // Put the filter in the steady state it would reach for a constant input x0
  // (avoids long start-up transients on DC-heavy signals such as gravity).
  void prime(float x0) {
    double num = (double)b0 + b1 + b2, den = 1.0 + a1 + a2;
    float y0 = (fabs(den) > 1e-12) ? (float)(num / den * x0) : 0.0f;
    z2 = b2 * x0 - a2 * y0;
    z1 = y0 - b0 * x0;
  }

  void design(int type, double fs, double fc, double Q) {  // 0 = LP, 1 = HP
    double w0 = 2.0 * M_PI * fc / fs;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * Q);
    double a0 = 1.0 + alpha;
    double B0, B1, B2;
    if (type == 0) { B0 = (1.0 - cw) / 2.0; B1 = 1.0 - cw; B2 = B0; }
    else { B0 = (1.0 + cw) / 2.0; B1 = -(1.0 + cw); B2 = B0; }
    b0 = (float)(B0 / a0); b1 = (float)(B1 / a0); b2 = (float)(B2 / a0);
    a1 = (float)((-2.0 * cw) / a0); a2 = (float)((1.0 - alpha) / a0);
  }
  void setLowpass(double fs, double fc, double Q = 0.70710678) { design(0, fs, fc, Q); }
  void setHighpass(double fs, double fc, double Q = 0.70710678) { design(1, fs, fc, Q); }
};

// 4th-order Butterworth made of two biquads (Q values of the 4th-order prototype)
struct Butter4 {
  Biquad s[2];
  void setLowpass(double fs, double fc) { s[0].setLowpass(fs, fc, 0.54119610); s[1].setLowpass(fs, fc, 1.30656296); }
  void setHighpass(double fs, double fc) { s[0].setHighpass(fs, fc, 0.54119610); s[1].setHighpass(fs, fc, 1.30656296); }
  inline float process(float x) { return s[1].process(s[0].process(x)); }
  void reset() { s[0].reset(); s[1].reset(); }
  void prime(float x0) {
    s[0].prime(x0);
    double num = (double)s[0].b0 + s[0].b1 + s[0].b2, den = 1.0 + s[0].a1 + s[0].a2;
    s[1].prime((float)(num / den * x0));
  }
};

// Band-pass = 4th-order HP followed by 4th-order LP (8th order total)
struct BandPass8 {
  Butter4 hp, lp;
  void design(double fs, double fLo, double fHi) { hp.setHighpass(fs, fLo); lp.setLowpass(fs, fHi); }
  void prime(float x0) { hp.prime(x0); lp.reset(); }
  inline float process(float x) { return lp.process(hp.process(x)); }
  void reset() { hp.reset(); lp.reset(); }
};

// ---------------------------------------------------------------------
// Moving average with exact running sum (re-summed periodically so the
// float accumulator can never drift).
// ---------------------------------------------------------------------
template <int N>
struct MovingAverage {
  float buf[N];
  float sum = 0;
  int idx = 0, count = 0, resum = 0;
  MovingAverage() { reset(); }
  void reset() { memset(buf, 0, sizeof(buf)); sum = 0; idx = 0; count = 0; resum = 0; }
  inline float push(float x) {
    sum += x - buf[idx];
    buf[idx] = x;
    idx = (idx + 1) % N;
    if (count < N) count++;
    if (++resum >= 4096) {
      resum = 0; double s = 0;
      for (int i = 0; i < N; i++) s += buf[i];
      sum = (float)s;
    }
    return sum / (float)N;
  }
  static constexpr float delay() { return (N - 1) * 0.5f; }  // group delay in samples
};

// ---------------------------------------------------------------------
// Radix-2 complex FFT (in place) with one shared cosine table (1024 pts).
// Supported sizes: 16 .. 1024 (power of two).
// ---------------------------------------------------------------------
#define DSP_FFT_MAX 1024
class DspFFT {
 public:
  static void init() {
    if (s_ready) return;
    for (int k = 0; k < DSP_FFT_MAX; k++) s_cos[k] = (float)cos(2.0 * M_PI * k / DSP_FFT_MAX);
    s_ready = true;
  }
  static inline float cosT(int k) { return s_cos[k & (DSP_FFT_MAX - 1)]; }            // cos(2*pi*k/1024)
  static inline float sinT(int k) { return s_cos[(k - DSP_FFT_MAX / 4) & (DSP_FFT_MAX - 1)]; }  // sin(2*pi*k/1024)

  // Periodic Hann window value for index i of length n (n divides 1024)
  static inline float hann(int i, int n) { return 0.5f - 0.5f * cosT(i * (DSP_FFT_MAX / n)); }

  static void fft(float* re, float* im, int n) {
    init();
    // bit reversal
    for (int i = 1, j = 0; i < n; i++) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
      int half = len >> 1;
      int step = DSP_FFT_MAX / len;
      for (int i = 0; i < n; i += len) {
        for (int k = 0; k < half; k++) {
          float wr = cosT(k * step), wi = -sinT(k * step);
          int a = i + k, b = a + half;
          float xr = re[b] * wr - im[b] * wi;
          float xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr; im[b] = im[a] - xi;
          re[a] += xr;        im[a] += xi;
        }
      }
    }
  }

 private:
  static float s_cos[DSP_FFT_MAX];
  static bool s_ready;
};

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------
// Parabolic interpolation of a peak at index i given neighbours: returns offset in [-0.5, 0.5]
static inline float dspParabolic(float ym1, float y0, float yp1) {
  float d = ym1 - 2.0f * y0 + yp1;
  if (fabsf(d) < 1e-12f) return 0.0f;
  return dspClamp(0.5f * (ym1 - yp1) / d, -0.5f, 0.5f);
}

// In-place insertion sort (small arrays only)
static inline void dspSort(float* a, int n) {
  for (int i = 1; i < n; i++) {
    float v = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}

// Median of n values using a scratch buffer (n <= cap)
static inline float dspMedian(const float* a, int n, float* scratch) {
  if (n <= 0) return NAN;
  memcpy(scratch, a, sizeof(float) * n);
  dspSort(scratch, n);
  return (n & 1) ? scratch[n / 2] : 0.5f * (scratch[n / 2 - 1] + scratch[n / 2]);
}

// Principal eigenvector of a symmetric 3x3 matrix by power iteration,
// seeded with (and sign-aligned to) the previous vector v.
static inline void dspPowerIter3(const float C[6], float v[3], int iters) {
  // C packed: xx, yy, zz, xy, xz, yz
  float x = v[0], y = v[1], z = v[2];
  for (int it = 0; it < iters; it++) {
    float nx = C[0] * x + C[3] * y + C[4] * z;
    float ny = C[3] * x + C[1] * y + C[5] * z;
    float nz = C[4] * x + C[5] * y + C[2] * z;
    float n = sqrtf(nx * nx + ny * ny + nz * nz);
    if (n < 1e-20f) return;  // matrix ~ 0, keep previous
    x = nx / n; y = ny / n; z = nz / n;
  }
  if (x * v[0] + y * v[1] + z * v[2] < 0) { x = -x; y = -y; z = -z; }
  v[0] = x; v[1] = y; v[2] = z;
}

// Robust covariance update: a sample whose energy exceeds clip x trace(C)
// is scaled down to that bound, so short artefacts (e.g. the first samples
// of a movement, before motion gating reacts) cannot rotate the principal
// axis away from the physiological signal.
static inline void dspCovUpdate(float C[6], const float x[3], float k);
static inline void dspCovUpdateRobust(float C[6], const float x[3], float k, float clip) {
  float e = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
  float tr = C[0] + C[1] + C[2];
  if (tr > 0 && e > clip * tr) {
    float s = sqrtf(clip * tr / e);
    float y[3] = {x[0] * s, x[1] * s, x[2] * s};
    dspCovUpdate(C, y, k);
  } else {
    dspCovUpdate(C, x, k);
  }
}

static inline void dspCovUpdate(float C[6], const float x[3], float k) {
  C[0] += k * (x[0] * x[0] - C[0]);
  C[1] += k * (x[1] * x[1] - C[1]);
  C[2] += k * (x[2] * x[2] - C[2]);
  C[3] += k * (x[0] * x[1] - C[3]);
  C[4] += k * (x[0] * x[2] - C[4]);
  C[5] += k * (x[1] * x[2] - C[5]);
}
