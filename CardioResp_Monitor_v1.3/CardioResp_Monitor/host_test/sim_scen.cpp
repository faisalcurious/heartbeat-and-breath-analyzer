// =====================================================================
//  sim_scen.cpp  -  v1.3 scenario simulator
//
//  Reproduces the field problems reported on v1.2 and checks the fixes:
//    * fast breathing (40-70 /min) must raise the breathing rate, not the
//      heart rate
//    * deep breaths must be drawn and counted, not flattened as movement
//    * arm movement, taps on the sensor, rubbing and talking must not be
//      counted as coughs, while real coughs still are
//
//  Body model -> virtual MPU6050 (random mounting, gyro bias, accel scale
//  error, noise, 44 Hz DLPF, 16-bit quantisation, +-2 g / +-250 dps) ->
//  the unchanged firmware VitalsEngine. Every episode is scored.
//
//  g++ -O2 -std=c++17 -I../CardioRespMonitor sim_scen.cpp ../CardioRespMonitor/VitalsEngine.cpp -o sim_scen
//  ./sim_scen            all scenarios          ./sim_scen -v   per-second log
// =====================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <string>
#include <cmath>
#include <algorithm>
#include "VitalsEngine.h"

namespace sim {
struct Mat3 { double m[3][3]; };
static Mat3 matMul(const Mat3& a, const Mat3& b) { Mat3 r{}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { double s = 0; for (int k = 0; k < 3; k++) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; } return r; }
static void matTVec(const Mat3& a, const double v[3], double o[3]) { for (int i = 0; i < 3; i++) o[i] = a.m[0][i] * v[0] + a.m[1][i] * v[1] + a.m[2][i] * v[2]; }
static Mat3 axisAngle(double ax, double ay, double az, double ang) {
  double n = sqrt(ax * ax + ay * ay + az * az);
  Mat3 I{}; I.m[0][0] = I.m[1][1] = I.m[2][2] = 1;
  if (n < 1e-12) return I;
  ax /= n; ay /= n; az /= n; double c = cos(ang), s = sin(ang), t = 1 - c;
  Mat3 r{{{t * ax * ax + c, t * ax * ay - s * az, t * ax * az + s * ay}, {t * ax * ay + s * az, t * ay * ay + c, t * ay * az - s * ax}, {t * ax * az - s * ay, t * ay * az + s * ax, t * az * az + c}}};
  return r;
}
static void orthonormalize(Mat3& R) {
  double c0[3] = {R.m[0][0], R.m[1][0], R.m[2][0]}, c1[3] = {R.m[0][1], R.m[1][1], R.m[2][1]};
  double n0 = sqrt(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]); for (int i = 0; i < 3; i++) c0[i] /= n0;
  double d = c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2]; for (int i = 0; i < 3; i++) c1[i] -= d * c0[i];
  double n1 = sqrt(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]); for (int i = 0; i < 3; i++) c1[i] /= n1;
  double c2[3] = {c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0]};
  for (int i = 0; i < 3; i++) { R.m[i][0] = c0[i]; R.m[i][1] = c1[i]; R.m[i][2] = c2[i]; }
}
}  // namespace sim
using namespace sim;

enum EpType { EP_FAST = 1, EP_DEEP, EP_HOLD, EP_ARM, EP_TAP, EP_COUGH, EP_RUB, EP_TURN, EP_TALK };
static const char* EPN[] = {"", "fast breathing", "deep breathing", "breath hold", "arm movement", "tap on sensor", "cough", "rubbing", "turning over", "talking"};
struct Episode { EpType type; double t0, t1; double p1, p2, p3, p4; };

struct Subject {
  const char* name;
  float age; double hr, rr, scgMg, gcgDps, breathDeg, dispMm;
  bool upright; unsigned seed; double T;
  std::vector<Episode> eps;
};

struct Sec { double t; VitalsResults r; };
struct RunOut {
  std::vector<Sec> sec;
  std::vector<double> beatT, breathT;   // ground truth
  std::vector<double> brValidT;         // engine: end time of every VALID breath
  std::vector<double> pauseT, motionT;  // engine events (start times)
  std::vector<double> motionDur;
  double tStart = 8.5;
};

static int g_verbose = 0;

static RunOut runSubject(const Subject& sb) {
  const double fs = 200.0, dt = 1.0 / fs, T = sb.T;
  const size_t NS = (size_t)(T * fs) + 2;
  std::mt19937 rng(sb.seed);
  std::normal_distribution<double> N01(0, 1);
  std::uniform_real_distribution<double> U(-1, 1);
  RunOut out;

  // ---------------- pre-pass: breathing phase, amplitude and heart rate multiplier per sample
  std::vector<double> ph(NS), amp(NS), disp(NS), vibK(NS), hrMul(NS);
  {
    double phase = 0, fS = sb.rr / 60.0, aS = sb.breathDeg, dS = sb.dispMm, vS = 0.25, hS = 1.0, drift = 0;
    int lastCycle = 0;
    for (size_t k = 0; k < NS; k++) {
      const double t = k * dt;
      double f = sb.rr / 60.0, A = sb.breathDeg, D = sb.dispMm, vk = 0.25, hm = 1.0; bool hold = false;
      for (const Episode& e : sb.eps) {
        if (t < e.t0 || t >= e.t1) continue;
        if (e.type == EP_FAST) { f = e.p1 / 60.0; A = e.p2; D = e.p3; vk = e.p4 > 0 ? e.p4 : 1.6; hm = 1.12; }
        else if (e.type == EP_DEEP) { f = e.p1 / 60.0; A = e.p2; D = e.p3; vk = 0.35; hm = 1.03; }
        else if (e.type == EP_HOLD) hold = true;
      }
      const double kf = dt / 1.2;                       // breathing pattern changes within about a second
      fS += kf * (f - fS); aS += kf * (A - aS); dS += kf * (D - dS); vS += kf * (vk - vS); hS += (dt / 6.0) * (hm - hS);
      if (k % 200 == 0) drift = 0.97 * drift + 0.03 * N01(rng);
      if (!hold) phase += 2 * M_PI * fS * (1.0 + 0.06 * drift) * dt;
      ph[k] = phase; amp[k] = aS; disp[k] = dS; vibK[k] = vS; hrMul[k] = hS;
      int cyc = (int)floor(phase / (2 * M_PI));
      if (cyc > lastCycle) { out.breathT.push_back(t); lastCycle = cyc; }
    }
  }
  auto shape = [](double p) {             // 0..1, inspiration 40 % of the cycle
    double x = fmod(p, 2 * M_PI) / (2 * M_PI);
    if (x < 0.4) return 0.5 - 0.5 * cos(M_PI * x / 0.4);
    return 0.5 + 0.5 * cos(M_PI * (x - 0.4) / 0.6);
  };
  // ---------------- ground-truth heart beats (sinus arrhythmia follows the breathing phase)
  {
    double t = 0.7;
    while (t < T) {
      out.beatT.push_back(t);
      size_t k = std::min(NS - 1, (size_t)(t * fs));
      double ibi = 60.0 / (sb.hr * hrMul[k]) * (1.0 + 0.035 * sin(ph[k])) + 0.012 * 60.0 / sb.hr * N01(rng);
      t += ibi;
    }
  }

  // ---------------- sensor + engine
  Mat3 S = axisAngle(U(rng), U(rng), U(rng), M_PI * (U(rng) + 1.0));             // random mounting
  Mat3 R = sb.upright ? axisAngle(1, 0, 0, M_PI / 2) : axisAngle(0, 0, 1, 0.3);   // body -> world
  double gyroBias[3] = {1.2, -0.8, 0.5}, accScale = 1.012;
  Biquad dl[6]; for (int i = 0; i < 6; i++) dl[i].setLowpass(200.0, 44.0);
  // band-limited noise sources (unit RMS approx.): breathing turbulence 6-40 Hz, cough / voice 15-45 Hz
  Biquad bvHp[2], bvLp[2], hfHp[2], hfLp[2];
  for (int i = 0; i < 2; i++) { bvHp[i].setHighpass(200.0, 6.0); bvLp[i].setLowpass(200.0, 40.0); hfHp[i].setHighpass(200.0, 15.0); hfLp[i].setLowpass(200.0, 45.0); }
  const double bvNorm = 1.0 / sqrt((40.0 - 6.0) / 100.0), hfNorm = 1.0 / sqrt((45.0 - 15.0) / 100.0);

  VitalsEngine eng; eng.init();
  CalibAccumulator acc; acc.reset();
  bool calibrated = false;
  double prevTheta = 0, d1 = 0, d2 = 0;
  int next = 0;
  for (size_t k = 0; k + 2 < NS; k++) {
    const double t = k * dt;
    // ---- breathing tilt + cough tilt (degrees)
    double theta = amp[k] * shape(ph[k]);
    double coughTilt = 0;
    for (const Episode& e : sb.eps) if (e.type == EP_COUGH) {
      double u = t - e.t0;
      if (u < 0 || u > 2.2) continue;
      if (u < 0.5) coughTilt += 2.5 * (0.5 - 0.5 * cos(M_PI * u / 0.5));            // quick deep inspiration
      else if (u < 0.65) coughTilt += 2.5;                                           // compression (glottis closed)
      else if (u < 0.95) coughTilt += 2.5 - 4.0 * (0.5 - 0.5 * cos(M_PI * (u - 0.65) / 0.3));   // explosive expiration
      else coughTilt += -1.5 * (1.0 - (u - 0.95) / 1.25);                            // recovery
    }
    theta += coughTilt;
    double thetaDot = (k == 0) ? 0 : (theta - prevTheta) / dt; prevTheta = theta;   // deg/s
    double wb[3] = {thetaDot * M_PI / 180.0, 0, 0};                                 // rad/s, rotation about body x
    double ab[3] = {0, 0, 0};                                                       // body-frame dynamic accel (g)
    // chest displacement along the chest normal -> linear acceleration
    {
      double d = disp[k] * shape(ph[k]) * 1e-3;                                      // m
      if (k >= 2) ab[2] += ((d - 2 * d1 + d2) / (dt * dt)) / 9.81;
      d2 = d1; d1 = d;
    }
    // breathing turbulence / muscle vibration, proportional to the chest rotation speed
    {
      double nz = bvLp[0].process(bvHp[0].process(N01(rng))) * bvNorm, ny = bvLp[1].process(bvHp[1].process(N01(rng))) * bvNorm;
      double a = vibK[k] * fabs(thetaDot) * 1e-3;                                     // g
      ab[2] += a * nz; ab[1] += 0.5 * a * ny;
      wb[0] += 0.06 * vibK[k] * fabs(thetaDot) * ny * M_PI / 180.0;
    }
    // heart: SCG / GCG bursts (same model as sim_test.cpp)
    while (next < (int)out.beatT.size() && out.beatT[next] < t - 1.0) next++;
    for (int b = next; b < (int)out.beatT.size() && out.beatT[b] < t + 0.05; b++) {
      double u = t - out.beatT[b];
      if (u >= 0 && u < 0.25) {
        double env = exp(-u / 0.030) * (u / 0.008 < 1 ? u / 0.008 : 1);
        double v = env * sin(2 * M_PI * 22.0 * u);
        ab[2] += sb.scgMg * 1e-3 * v; ab[1] += 0.35 * sb.scgMg * 1e-3 * v;
        wb[0] += sb.gcgDps * M_PI / 180 * env * sin(2 * M_PI * 15.0 * u + 0.5);
        wb[2] += 0.5 * sb.gcgDps * M_PI / 180 * env * sin(2 * M_PI * 15.0 * u);
      }
      double u2 = u - 0.34;
      if (u2 >= 0 && u2 < 0.2) {
        double env = exp(-u2 / 0.025) * (u2 / 0.006 < 1 ? u2 / 0.006 : 1);
        ab[2] += 0.45 * sb.scgMg * 1e-3 * env * sin(2 * M_PI * 18.0 * u2);
        wb[0] += 0.4 * sb.gcgDps * M_PI / 180 * env * sin(2 * M_PI * 12.0 * u2);
      }
    }
    // ---- episodes: movement, taps, coughs, rubbing, talking, turning
    double aw[3] = {0, 0, 0};
    double hfz = hfLp[0].process(hfHp[0].process(N01(rng))) * hfNorm, hfy = hfLp[1].process(hfHp[1].process(N01(rng))) * hfNorm;
    for (const Episode& e : sb.eps) {
      const double u = t - e.t0, dur = e.t1 - e.t0;
      switch (e.type) {
        case EP_ARM: if (u >= 0 && u < dur) {        // p1 mg linear, p2 deg/s rotation, 1.5-4 Hz
          double w = sin(M_PI * u / dur); w *= w;
          aw[0] += e.p1 * 1e-3 * w * (0.6 * sin(2 * M_PI * 2.1 * u) + 0.4 * sin(2 * M_PI * 3.3 * u + 1));
          aw[2] += 0.7 * e.p1 * 1e-3 * w * sin(2 * M_PI * 1.6 * u + 0.4);
          wb[0] += e.p2 * M_PI / 180 * w * sin(2 * M_PI * 1.7 * u); wb[1] += 0.7 * e.p2 * M_PI / 180 * w * sin(2 * M_PI * 2.6 * u + 0.5);
          wb[2] += 0.5 * e.p2 * M_PI / 180 * w * sin(2 * M_PI * 1.1 * u);
          ab[2] += 1.5e-3 * w * hfz;                                                  // muscle tremor
        } break;
        case EP_TAP: if (u >= 0 && u < 0.08) {       // p1 g peak, decays in a few ms (finger on the sensor)
          ab[2] += e.p1 * exp(-u / 0.006) * sin(2 * M_PI * 38.0 * u);
          ab[0] += 0.4 * e.p1 * exp(-u / 0.006) * sin(2 * M_PI * 31.0 * u + 0.3);
          wb[1] += 8.0 * M_PI / 180 * exp(-u / 0.012) * sin(2 * M_PI * 20.0 * u);
        } break;
        case EP_COUGH: {                              // p1 mg RMS broadband burst, p2 g trunk jerk
          double v = u - 0.62;
          if (v >= 0 && v < 0.35) ab[2] += e.p2 * sin(M_PI * v / 0.35) * sin(2 * M_PI * 4.0 * v);
          double h = u - 0.65;
          if (h >= 0 && h < 0.55) {
            double env = (h < 0.02 ? h / 0.02 : 1.0) * exp(-h / 0.13);
            ab[2] += e.p1 * 1e-3 * env * hfz; ab[1] += 0.6 * e.p1 * 1e-3 * env * hfy;
            wb[0] += 22.0 * M_PI / 180 * exp(-h / 0.09) * sin(2 * M_PI * 5.0 * h);
          }
        } break;
        case EP_RUB: if (u >= 0 && u < dur) {         // p1 mg at 5 Hz + p2 mg broadband (scratching near the sensor)
          ab[0] += e.p1 * 1e-3 * sin(2 * M_PI * 5.0 * u); ab[2] += e.p2 * 1e-3 * hfz;
          wb[2] += 3.0 * M_PI / 180 * sin(2 * M_PI * 5.0 * u);
        } break;
        case EP_TALK: if (u >= 0 && u < dur) {        // p1 mg voice vibration, syllables at 4 Hz
          double m = 0.5 + 0.5 * sin(2 * M_PI * 4.0 * u);
          ab[2] += e.p1 * 1e-3 * m * hfz; ab[1] += 0.5 * e.p1 * 1e-3 * m * hfy;
        } break;
        case EP_TURN: if (u >= 0 && u < dur) {        // p1 degrees about the body axis in dur seconds
          wb[1] += e.p1 * M_PI / 180 * (M_PI / 2) / dur * sin(M_PI * u / dur);
          aw[0] += 0.15 * sin(M_PI * u / dur) * sin(2 * M_PI * 1.2 * u);
        } break;
        default: break;
      }
    }
    // ---- integrate body orientation, sensor outputs
    double wn = sqrt(wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2]);
    Mat3 dR = axisAngle(wb[0], wb[1], wb[2], wn * dt);
    R = matMul(R, dR); if (k % 1000 == 0) orthonormalize(R);
    double up[3] = {aw[0], aw[1], aw[2] + 1.0}, fb[3]; matTVec(R, up, fb);
    for (int i = 0; i < 3; i++) fb[i] += ab[i];
    double fs_[3], ws[3]; matTVec(S, fb, fs_); matTVec(S, wb, ws);
    float v[6];
    for (int i = 0; i < 3; i++) {
      double a = fs_[i] * accScale + 0.0030 * N01(rng);
      double g = ws[i] * 180 / M_PI + gyroBias[i] + 0.05 * N01(rng);
      a = dl[i].process((float)a); g = dl[3 + i].process((float)g);
      a = std::max(-2.0, std::min(2.0, a)); g = std::max(-250.0, std::min(250.0, g));
      v[i] = (float)(lrint(a * 16384.0) / 16384.0);
      v[3 + i] = (float)(lrint(g * 131.0) / 131.0);
    }
    if (!calibrated) {
      if (t >= 0.5) acc.add(v[0], v[1], v[2], v[3], v[4], v[5]);
      if (t >= out.tStart) {
        CalibData c; char msg[96];
        acc.finish(c, msg, sizeof(msg));
        eng.begin(c, sb.age);
        calibrated = true;
      }
      continue;
    }
    eng.pushSample(v[0], v[1], v[2], v[3], v[4], v[5]);
    if ((k % 200) == 0) {
      eng.analyze();
      Sec s; s.t = t; eng.getResults(s.r);
      out.sec.push_back(s);
      if (g_verbose > 1)
        printf("    t=%6.1f HR %5.1f inst %5.1f | RRnow %5.1f RR1 %5.1f RR5 %5.1f | mot %d | cough30 %d | pres %d sqiH %3d sqiR %3d\n", t, s.r.hr1m, s.r.hrInst,
#ifdef VE_HAS_RRNOW
#define HR_ART(r) ((r).hrArtefact)
#else
#define HR_ART(r) false            // v1.2 engine (comparison runs): no artefact flag
#endif
#ifdef VE_HAS_RRNOW
               s.r.rrNow,
#else
               NAN,
#endif
               s.r.rr1m, s.r.rr5m, s.r.motion ? 1 : 0, s.r.cough30, s.r.heartPresent ? 1 : 0, s.r.sqiH, s.r.sqiR);
    }
  }
  for (uint32_t i = eng.breaths.oldest(); i < eng.breaths.head; i++) {
    const BreathEvent& b = eng.breaths.at(i);
    if (b.flags & BR_VALID) out.brValidT.push_back(out.tStart + b.n10 / 10.0);
  }
  for (uint32_t i = eng.events.oldest(); i < eng.events.head; i++) {
    const VEvent& e = eng.events.at(i);
    if (e.type == EV_PAUSE) out.pauseT.push_back(out.tStart + e.tMs / 1000.0);
    if (e.type == EV_MOTION) { out.motionT.push_back(out.tStart + e.tMs / 1000.0); out.motionDur.push_back(e.durMs / 1000.0); }
  }
  return out;
}

// ---------------------------------------------------------------- scoring helpers
static int countIn(const std::vector<double>& v, double a, double b) { int n = 0; for (double x : v) if (x >= a && x < b) n++; return n; }
static double trueHrAround(const RunOut& o, double a, double b) {
  std::vector<double> ib; for (size_t i = 1; i < o.beatT.size(); i++) if (o.beatT[i] >= a && o.beatT[i] < b) ib.push_back(o.beatT[i] - o.beatT[i - 1]);
  double m = 0; for (double x : ib) m += x; return ib.empty() ? NAN : 60.0 / (m / ib.size());
}
static uint32_t coughsUpTo(const RunOut& o, double t) {     // cumulative detected coughs at time t
  uint32_t c = 0;
  for (const Sec& s : o.sec) { if (s.t > t) break; c = s.r.coughTotal; }
  return c;
}
static double motionFrac(const RunOut& o, double a, double b) { int n = 0, m = 0; for (const Sec& s : o.sec) if (s.t >= a && s.t < b) { n++; if (s.r.motion) m++; } return n ? (double)m / n : 0; }
static double rrShown(const VitalsResults& r) {
#ifdef VE_HAS_RRNOW
  if (!std::isnan(r.rrNow)) return r.rrNow;
#endif
  return r.rr1m;
}

struct Check { int fails = 0, total = 0; void ok(bool c, const char* fmt, ...) __attribute__((format(printf, 3, 4))); };
#include <cstdarg>
void Check::ok(bool c, const char* fmt, ...) {
  total++; if (!c) fails++;
  va_list ap; va_start(ap, fmt); printf("    %s ", c ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n"); va_end(ap);
}

static void scoreEpisodes(const Subject& sb, const RunOut& o, Check& C) {
  for (const Episode& e : sb.eps) {
    switch (e.type) {
      case EP_FAST: {
        // breathing rate shown during the last 15 s of the episode, heart rate during the whole episode
        double rrMin = 1e9, rrMax = 0, hrErrMax = 0, hrMax = 0; int n = 0, nh = 0, lockN = 0;
        const double hrT = trueHrAround(o, e.t0 + 5, e.t1);
        for (const Sec& s : o.sec) {
          if (s.t >= e.t1 - 15 && s.t < e.t1) { double v = rrShown(s.r); if (!std::isnan(v)) { rrMin = std::min(rrMin, v); rrMax = std::max(rrMax, v); n++; } }
          if (s.t >= e.t0 && s.t < e.t1 + 20 && !std::isnan(s.r.hr1m)) {
            nh++; hrErrMax = std::max(hrErrMax, fabs(s.r.hr1m - hrT)); hrMax = std::max(hrMax, (double)s.r.hr1m);
            if (!std::isnan(s.r.hrInst) && fabs(s.r.hrInst - e.p1) < 0.08 * e.p1) lockN++;
          }
        }
        int trueBr = countIn(o.breathT, e.t0 + 3, e.t1), detBr = countIn(o.brValidT, e.t0 + 3, e.t1);
        printf("  %s %.0f-%.0f s at %.0f /min: breathing shown %.1f-%.1f /min (n=%d), breaths %d of %d counted, motion %.0f %%, HR true %.1f, shown max %.1f (max err %.1f), HR locked on breathing %d s\n",
               EPN[e.type], e.t0, e.t1, e.p1, n ? rrMin : NAN, n ? rrMax : NAN, n, detBr, trueBr, 100 * motionFrac(o, e.t0, e.t1), hrT, hrMax, hrErrMax, lockN);
        C.ok(n >= 10 && rrMin >= 0.80 * e.p1 && rrMax <= 1.2 * e.p1, "breathing rate follows the fast breathing (%.0f /min)", e.p1);
        C.ok(detBr >= 0.80 * trueBr, "at least 80 %% of the fast breaths are counted (%d / %d)", detBr, trueBr);
        C.ok(hrErrMax < 12.0, "heart rate stays within 12 bpm of the true value (max error %.1f)", hrErrMax);
        C.ok(lockN <= 2, "heart rate does not lock onto the breathing rhythm (%d s)", lockN);
        if (e.t1 - e.t0 >= 120 && e.p4 > 0 && e.p4 < 0.5) {     // long, quiet fast breathing (e.g. pneumonia): heart rate must stay available
          int nAv = 0, nT = 0; for (const Sec& s : o.sec) if (s.t >= e.t0 + 30 && s.t < e.t1) { nT++; if (!std::isnan(s.r.hr1m) && !HR_ART(s.r)) nAv++; }
          C.ok(nT && nAv >= 0.9 * nT, "heart rate stays available during quiet fast breathing (%d of %d s)", nAv, nT);
        }
      } break;
      case EP_DEEP: {
        int trueBr = countIn(o.breathT, e.t0 + 2, e.t1), detBr = countIn(o.brValidT, e.t0 + 2, e.t1 + 2);
        double mf = motionFrac(o, e.t0, e.t1);
        double until = e.t1 + 40;                               // up to 40 s after, or until the next scripted episode
        for (const Episode& x : sb.eps) if (x.t0 > e.t0 && x.t0 < until) until = x.t0;
        int falseP = countIn(o.pauseT, e.t0 - 2, until);
        printf("  %s %.0f-%.0f s (%.0f /min, %.1f deg): motion %.0f %%, breaths %d of %d counted, pauses %d\n", EPN[e.type], e.t0, e.t1, e.p1, e.p2, 100 * mf, detBr, trueBr, falseP);
        C.ok(mf < 0.10, "deep breaths are not treated as movement (%.0f %% of the time)", 100 * mf);
        C.ok(detBr >= trueBr - 1, "deep breaths are counted (%d / %d)", detBr, trueBr);
        C.ok(falseP == 0, "no false breathing pause during or after the deep breaths");
      } break;
      case EP_HOLD: {
        int p = countIn(o.pauseT, e.t0 - 2, e.t1 + 5);
        printf("  %s %.0f-%.0f s: pause events %d\n", EPN[e.type], e.t0, e.t1, p);
        if (e.t1 - e.t0 >= ageProfileFor(sb.age).apneaSec + 3) C.ok(p >= 1, "the breath hold is reported as a breathing pause");
      } break;
      case EP_COUGH: {
        uint32_t c0 = coughsUpTo(o, e.t0 - 0.5), c1 = coughsUpTo(o, e.t0 + 3.5);
        printf("  %s at %.1f s: detected %u\n", EPN[e.type], e.t0, c1 - c0);
      } break;
      case EP_ARM: case EP_TAP: case EP_RUB: case EP_TALK: case EP_TURN: {
        uint32_t c0 = coughsUpTo(o, e.t0 - 0.5), c1 = coughsUpTo(o, e.t1 + 3.5);
        printf("  %s %.1f-%.1f s: coughs counted %u, motion flagged %s\n", EPN[e.type], e.t0, e.t1, c1 - c0, countIn(o.motionT, e.t0 - 1, e.t1 + 3) ? "yes" : "no");
        C.ok(c1 == c0, "%s at %.0f s is not counted as a cough", EPN[e.type], e.t0);
      } break;
      default: break;
    }
  }
  // real coughs: count all cough episodes together
  int nC = 0; uint32_t det = 0;
  for (const Episode& e : sb.eps) if (e.type == EP_COUGH) { nC++; det += coughsUpTo(o, e.t0 + 3.5) - coughsUpTo(o, e.t0 - 0.5); }
  if (nC) C.ok(det >= (uint32_t)std::max(1, (int)lround(0.7 * nC)) && det <= (uint32_t)(nC + 1), "real coughs detected: %u of %d", det, nC);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) g_verbose = 2;
  const char* only = nullptr;
  for (int i = 1; i < argc; i++) if (argv[i][0] != '-') only = argv[i];
  std::vector<Subject> list;
  // 1) the reported test: quiet breathing, then fast breathing for 40 s, then quiet again
  list.push_back({"Fast breathing 45 /min for 40 s (sternum, lying)", 30, 74, 14, 12, 1.5, 1.2, 4, false, 41, 240,
                  {{EP_FAST, 120, 160, 45, 1.0, 5, 0}}});
  list.push_back({"Very fast panting 65 /min for 30 s (sitting)", 30, 78, 15, 10, 1.2, 1.0, 4, true, 42, 240,
                  {{EP_FAST, 120, 150, 65, 0.8, 4, 0}}});
  list.push_back({"Fast breathing 36 /min, weak heart signal (side of chest)", 50, 66, 13, 5, 0.6, 1.3, 4, true, 43, 240,
                  {{EP_FAST, 110, 160, 36, 1.4, 6, 0}}});
  // 2) deep breaths
  list.push_back({"Five deep breaths (7 deg, 6 /min)", 30, 72, 14, 12, 1.5, 1.2, 4, false, 44, 260,
                  {{EP_DEEP, 120, 170, 6, 7.0, 25, 0}}});
  list.push_back({"Deep slow breathing for 2 min (5 deg, 8 /min), sitting", 45, 68, 13, 10, 1.2, 1.0, 4, true, 45, 330,
                  {{EP_DEEP, 120, 240, 8, 5.0, 20, 0}}});
  list.push_back({"Very deep breaths (12 deg, 5 /min)", 30, 70, 14, 12, 1.5, 1.2, 4, false, 46, 260,
                  {{EP_DEEP, 120, 170, 5, 12.0, 35, 0}}});
  // 3) movement that must not be a cough, and real coughs
  list.push_back({"Arm movements (6 x 1.5 s)", 35, 72, 14, 12, 1.5, 1.2, 4, false, 47, 260,
                  {{EP_ARM, 100, 101.5, 120, 8, 0}, {EP_ARM, 118, 119.5, 200, 12, 0}, {EP_ARM, 136, 137.2, 90, 6, 0},
                   {EP_ARM, 154, 155.5, 300, 15, 0}, {EP_ARM, 172, 173.0, 150, 10, 0}, {EP_ARM, 190, 191.5, 250, 20, 0}}});
  list.push_back({"Taps on the sensor and rubbing", 35, 72, 14, 12, 1.5, 1.2, 4, false, 48, 240,
                  {{EP_TAP, 100, 100.1, 0.6, 0, 0}, {EP_TAP, 115, 115.1, 1.2, 0, 0}, {EP_TAP, 130, 130.1, 0.4, 0, 0},
                   {EP_RUB, 150, 154, 40, 15, 0}, {EP_TALK, 175, 185, 8, 0, 0}, {EP_TURN, 205, 207.5, 35, 0, 0}}});
  list.push_back({"Real coughs (5 in 25 s)", 35, 76, 15, 12, 1.5, 1.2, 4, false, 49, 220,
                  {{EP_COUGH, 120, 122, 120, 0.25, 0}, {EP_COUGH, 126, 128, 90, 0.20, 0}, {EP_COUGH, 131, 133, 150, 0.30, 0},
                   {EP_COUGH, 137, 139, 100, 0.25, 0}, {EP_COUGH, 143, 145, 130, 0.25, 0}}});
  list.push_back({"Mixed: movement, then coughs, then fast breathing", 40, 74, 14, 11, 1.4, 1.2, 4, false, 50, 300,
                  {{EP_ARM, 100, 101.5, 180, 12, 0}, {EP_ARM, 110, 111.2, 120, 8, 0},
                   {EP_COUGH, 140, 142, 110, 0.25, 0}, {EP_COUGH, 147, 149, 120, 0.25, 0}, {EP_COUGH, 156, 158, 90, 0.2, 0},
                   {EP_FAST, 200, 240, 40, 1.1, 5, 0}}});
  list.push_back({"Quiet fast breathing 40 /min for 5 min (pneumonia-like)", 60, 96, 16, 10, 1.2, 1.0, 4, false, 51, 520,
                  {{EP_FAST, 120, 420, 40, 0.8, 3, 0.25}}});
  list.push_back({"Forceful fast breathing 50 /min, very strong air-flow vibration", 30, 75, 14, 12, 1.5, 1.2, 4, true, 52, 240,
                  {{EP_FAST, 120, 160, 50, 1.6, 6, 3.0}}});
  list.push_back({"Deep breaths taken quickly (8 deg, 12 /min) then a 15 s breath hold", 30, 70, 14, 12, 1.5, 1.2, 4, false, 53, 280,
                  {{EP_DEEP, 120, 170, 12, 8.0, 25, 0}, {EP_HOLD, 200, 215, 0, 0, 0, 0}}});
  list.push_back({"Infant 6 months: fast normal breathing 45 /min, kicks (arm-like)", 0.5, 135, 45, 6, 0.8, 0.7, 2, false, 54, 240,
                  {{EP_ARM, 110, 111.5, 150, 12, 0, 0}, {EP_ARM, 150, 151.0, 100, 10, 0, 0}}});
  Check C;
  for (const Subject& sb : list) {
    if (only && !strstr(sb.name, only)) continue;
    printf("=== %s ===\n", sb.name);
    RunOut o = runSubject(sb);
    const VitalsResults& r = o.sec.back().r;
    printf("  end: HR %.1f (true %.1f) RR5 %.1f RR1 %.1f | motion events %zu, pauses %zu, coughs %u\n", r.hr1m, trueHrAround(o, sb.T - 60, sb.T), r.rr5m, r.rr1m,
           o.motionT.size(), o.pauseT.size(), (unsigned)r.coughTotal);
    scoreEpisodes(sb, o, C);
  }
  printf("\n%d of %d checks passed\n", C.total - C.fails, C.total);
  return C.fails ? 1 : 0;
}
