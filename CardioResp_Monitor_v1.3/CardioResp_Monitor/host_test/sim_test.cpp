// =====================================================================
//  sim_test.cpp  -  PC verification of VitalsEngine
//
//  Builds a physiological body model (heart beats with sinus arrhythmia,
//  SCG linear vibration, GCG rotational vibration, breathing chest-wall
//  rotation, a breath-hold, a movement episode and a posture change),
//  mounts a virtual MPU6050 in a RANDOM orientation, adds realistic noise,
//  bias, scale error and 16-bit quantisation, then runs the exact firmware
//  engine and scores it against ground truth.
//
//  g++ -O2 -std=c++17 -I../CardioRespMonitor sim_test.cpp ../CardioRespMonitor/VitalsEngine.cpp -o sim
// =====================================================================
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>
#include <functional>
#include <cmath>
#include "VitalsEngine.h"

struct Mat3 { double m[3][3]; };
static Mat3 matMul(const Mat3& a, const Mat3& b) { Mat3 r{}; for (int i=0;i<3;i++) for(int j=0;j<3;j++){double s=0; for(int k=0;k<3;k++) s+=a.m[i][k]*b.m[k][j]; r.m[i][j]=s;} return r; }
static void matVec(const Mat3& a, const double v[3], double o[3]) { for (int i=0;i<3;i++) o[i]=a.m[i][0]*v[0]+a.m[i][1]*v[1]+a.m[i][2]*v[2]; }
static void matTVec(const Mat3& a, const double v[3], double o[3]) { for (int i=0;i<3;i++) o[i]=a.m[0][i]*v[0]+a.m[1][i]*v[1]+a.m[2][i]*v[2]; }
static Mat3 axisAngle(double ax, double ay, double az, double ang) {
  double n = sqrt(ax*ax+ay*ay+az*az); if (n < 1e-12) { Mat3 I{}; I.m[0][0]=I.m[1][1]=I.m[2][2]=1; return I; }
  ax/=n; ay/=n; az/=n; double c=cos(ang), s=sin(ang), t=1-c;
  Mat3 r{{{t*ax*ax+c, t*ax*ay-s*az, t*ax*az+s*ay},{t*ax*ay+s*az, t*ay*ay+c, t*ay*az-s*ax},{t*ax*az-s*ay, t*ay*az+s*ax, t*az*az+c}}};
  return r;
}
static void orthonormalize(Mat3& R) {  // Gram-Schmidt on columns
  double c0[3]={R.m[0][0],R.m[1][0],R.m[2][0]}, c1[3]={R.m[0][1],R.m[1][1],R.m[2][1]};
  double n0=sqrt(c0[0]*c0[0]+c0[1]*c0[1]+c0[2]*c0[2]); for(int i=0;i<3;i++) c0[i]/=n0;
  double d=c0[0]*c1[0]+c0[1]*c1[1]+c0[2]*c1[2]; for(int i=0;i<3;i++) c1[i]-=d*c0[i];
  double n1=sqrt(c1[0]*c1[0]+c1[1]*c1[1]+c1[2]*c1[2]); for(int i=0;i<3;i++) c1[i]/=n1;
  double c2[3]={c0[1]*c1[2]-c0[2]*c1[1], c0[2]*c1[0]-c0[0]*c1[2], c0[0]*c1[1]-c0[1]*c1[0]};
  for(int i=0;i<3;i++){R.m[i][0]=c0[i];R.m[i][1]=c1[i];R.m[i][2]=c2[i];}
}

struct Scenario {
  const char* name;
  float age; double hr, rr;
  double scgMg, gcgDps, breathDeg;
  unsigned seed;
  bool bodyUpright;  // true: sitting (chest normal horizontal), false: supine
  double accBias[3] = {0, 0, 0};   // accelerometer offset (g), e.g. clone MPU
  double accDriftZ = 0;            // extra Z offset reached after 90 s warm-up (g)
  double ampStepT = 1e9, ampStepK = 1;  // breathing depth x K after time T (lying down)
  int breathAxis = 0;              // body axis of breathing rotation (0 = lateral, 2 = chest normal)
};

struct Score { double hrErr, rrErr, sens, ppv, rmssdTrue, rmssdEst; int pauses, motionEv, postureEv; bool ok; double rrCov, hrCov; int falsePauses; };

static double g_simT = 600.0;
static double g_heartStop = 1e9;            // v1.2 test hook: no heart beats after this time (asystole)
static std::vector<double> g_coughs;         // v1.2 test hook: cough burst start times (s)
static std::function<void(double, VitalsEngine&, bool)> g_hook;
static Score runScenario(const Scenario& sc, bool verbose) {
  const double fs = 200.0, dt = 1.0 / fs, T = g_simT;
  std::mt19937 rng(sc.seed);
  std::normal_distribution<double> N01(0, 1);
  std::uniform_real_distribution<double> U(-1, 1);

  // random sensor mounting (sensor->body)
  Mat3 S = axisAngle(U(rng), U(rng), U(rng), M_PI * (U(rng) + 1.0));
  // initial body orientation (body->world): body z = chest normal
  Mat3 R = sc.bodyUpright ? axisAngle(1, 0, 0, M_PI / 2) : axisAngle(0, 0, 1, 0.3);

  // ---- ground truth beats (with respiratory sinus arrhythmia)
  std::vector<double> beatT;
  { double t = 0.7; while (t < T && t < g_heartStop) { beatT.push_back(t); double ph = 2*M_PI*sc.rr/60.0*t;
      double ibi = 60.0/sc.hr * (1.0 + 0.035*sin(ph)) + 0.012*60.0/sc.hr*N01(rng); t += ibi; } }
  // ---- breathing phase with variability, breath hold 360-378 s
  const double holdA = 360, holdB = 378;
  std::vector<double> brPhase((size_t)(T * fs) + 2);
  { double ph = 0, f = sc.rr / 60.0, drift = 0;
    for (size_t k = 0; k < brPhase.size(); k++) { double t = k*dt;
      if (k % 200 == 0) drift = 0.97*drift + 0.03*N01(rng);
      if (!(t >= holdA && t < holdB)) ph += 2*M_PI*f*(1.0 + 0.08*drift)*dt;
      brPhase[k] = ph; } }
  auto breathShape = [](double ph) {   // 0..1, inspiration 40 % of the cycle
    double x = fmod(ph, 2*M_PI) / (2*M_PI);
    if (x < 0.4) return 0.5 - 0.5*cos(M_PI*x/0.4);
    return 0.5 + 0.5*cos(M_PI*(x-0.4)/0.6);
  };
  // count true breaths in last 300 s at end
  double gyroBias[3] = {1.2, -0.8, 0.5}, accScale = 1.012;
  Biquad dl[6]; for (int i = 0; i < 6; i++) dl[i].setLowpass(200.0, 44.0);   // MPU DLPF (approx.)

  VitalsEngine eng; eng.init();
  CalibAccumulator acc; acc.reset();
  bool calibrated = false;
  double prevTheta = 0;
  Score s{}; s.ok = true;
  int next = 0;  // next beat index for SCG synthesis window
  std::vector<double> trueBeatUsed;

  // motion episode 240-246 s, posture change 420-423 s
  const double motA = 240, motB = 246, posA = 420, posB = 423;
  double lastLog = 0;
  int covN = 0, rrOk = 0, hrOk = 0;
  for (size_t k = 0; k < (size_t)(T * fs); k++) {
    double t = k * dt;
    // breathing rotation about body x
    double theta = sc.breathDeg * (t >= sc.ampStepT ? sc.ampStepK : 1.0) * M_PI/180.0 * breathShape(brPhase[k]);
    if (t >= holdA && t < holdB) theta = sc.breathDeg * M_PI/180.0 * breathShape(brPhase[k]);
    double thetaDot = (k == 0) ? 0 : (theta - prevTheta) / dt; prevTheta = theta;
    double wb[3] = {0, 0, 0};
    wb[sc.breathAxis] = thetaDot;
    double ab[3] = {0, 0, 0};  // body-frame SCG accel (g)
    // SCG / GCG bursts
    while (next < (int)beatT.size() && beatT[next] < t - 1.0) next++;
    for (int b = next; b < (int)beatT.size() && beatT[b] < t + 0.05; b++) {
      double u = t - beatT[b];
      if (u >= 0 && u < 0.25) {
        double env = exp(-u / 0.030) * (u / 0.008 < 1 ? u / 0.008 : 1);
        double v = env * sin(2*M_PI*22.0*u);
        ab[2] += sc.scgMg * 1e-3 * v; ab[1] += 0.35 * sc.scgMg * 1e-3 * v;
        wb[0] += sc.gcgDps * M_PI/180 * env * sin(2*M_PI*15.0*u + 0.5);
        wb[2] += 0.5*sc.gcgDps * M_PI/180 * env * sin(2*M_PI*15.0*u);
      }
      double u2 = u - 0.34;
      if (u2 >= 0 && u2 < 0.2) {
        double env = exp(-u2 / 0.025) * (u2 / 0.006 < 1 ? u2 / 0.006 : 1);
        ab[2] += 0.45 * sc.scgMg * 1e-3 * env * sin(2*M_PI*18.0*u2);
        wb[0] += 0.4 * sc.gcgDps * M_PI/180 * env * sin(2*M_PI*12.0*u2);
      }
    }
    for (double tc : g_coughs) {            // cough: broadband chest-wall acceleration burst, 0.3 s
      double u = t - tc;
      if (u >= 0 && u < 0.4) { double env = exp(-u / 0.08) * (u / 0.01 < 1 ? u / 0.01 : 1); for (int q = 0; q < 3; q++) ab[q] += 0.30 * env * N01(rng); wb[0] += 25.0 * M_PI/180 * env * N01(rng); }
    }
    double aw[3] = {0, 0, 0};
    if (t >= motA && t < motB) {  // movement episode
      wb[0] += 1.2*sin(2*M_PI*1.3*t); wb[1] += 0.9*sin(2*M_PI*2.1*t + 1); wb[2] += 0.7*sin(2*M_PI*0.8*t);
      aw[0] += 0.25*sin(2*M_PI*1.7*t); aw[1] += 0.2*sin(2*M_PI*2.3*t); aw[2] += 0.15*sin(2*M_PI*3.1*t);
    }
    if (t >= posA && t < posB) wb[1] += (40.0 * M_PI/180) / (posB - posA);  // 40 deg posture change
    // integrate body orientation
    Mat3 dR = axisAngle(wb[0], wb[1], wb[2], sqrt(wb[0]*wb[0]+wb[1]*wb[1]+wb[2]*wb[2]) * dt);
    R = matMul(R, dR); if (k % 1000 == 0) orthonormalize(R);
    // specific force in body frame: R^T (a_world + g_up) + SCG
    double up[3] = {aw[0], aw[1], aw[2] + 1.0}, fb[3]; matTVec(R, up, fb);
    for (int i = 0; i < 3; i++) fb[i] += ab[i];
    double fs_[3], ws[3]; matTVec(S, fb, fs_); matTVec(S, wb, ws);
    float v[6];
    for (int i = 0; i < 3; i++) {
      double bias = sc.accBias[i] + (i == 2 ? sc.accDriftZ * std::min(1.0, t / 90.0) : 0.0);
      double a = fs_[i] * accScale + bias + 0.0030 * N01(rng);
      double g = ws[i] * 180/M_PI + gyroBias[i] + 0.05 * N01(rng);
      a = dl[i].process((float)a); g = dl[3+i].process((float)g);
      a = std::max(-2.0, std::min(2.0, a)); g = std::max(-250.0, std::min(250.0, g));
      v[i] = (float)(lrint(a * 16384.0) / 16384.0);
      v[3+i] = (float)(lrint(g * 131.0) / 131.0);
    }
    if (!calibrated) {
      if (t >= 0.5) acc.add(v[0], v[1], v[2], v[3], v[4], v[5]);
      if (t >= 8.5) {
        CalibData c; char msg[96];
        bool ok = acc.finish(c, msg, sizeof(msg));
        if (verbose) printf("  calibration: %s %s |g|=%.4f bias=(%.2f %.2f %.2f) std_g=(%.2f %.2f %.2f)\n", ok ? "OK" : "FAIL", msg, c.gMag,
                            c.gyroBias[0], c.gyroBias[1], c.gyroBias[2], c.gyroStd[0], c.gyroStd[1], c.gyroStd[2]);
        eng.begin(c, sc.age);
        calibrated = true;
      }
      continue;
    }
    eng.pushSample(v[0], v[1], v[2], v[3], v[4], v[5]);
    bool an = (k % 200) == 0;
    if (an) {
      eng.analyze();
      bool excl = t < 90 || (t > 238 && t < 262) || (t > 418 && t < 440) || (t > 355 && t < 395);
      if (!excl) {
        VitalsResults rr_; eng.getResults(rr_);
        covN++; if (!std::isnan(rr_.rr1m)) rrOk++; if (!std::isnan(rr_.hr1m)) hrOk++;
      }
    }
    if (g_hook) g_hook(t, eng, an);
    if (verbose && t - lastLog >= 60.0) {
      lastLog = t;
      VitalsResults r; eng.getResults(r);
      printf("  t=%5.0fs HR1m=%6.1f inst=%6.1f acf=%6.1f spec=%6.1f sqiH=%3d wa=%.2f qa=%.2f qg=%.2f | RR5=%5.1f RR1=%5.1f spec=%5.1f sqiR=%3d amp=%.2f IE=%.2f pol=%d tilt=%.0f snr=%.2f qf=%.2f lvl=%.3f\n",
             t, r.hr1m, r.hrInst, r.hrAcf, r.hrSpec, r.sqiH, r.wa, r.qa, r.qg, r.rr5m, r.rr1m, r.rrSpec, r.sqiR, r.ampMean, r.ieRatio, r.polarity, r.tiltFromCal, r.pulseSnr, r.qf, r.respLevel);
    }
  }
  // ---- scoring over the last minute / last 5 minutes
  VitalsResults r; eng.getResults(r);
  const double tStart = 8.5;  // engine time zero
  double tEnd = T;
  std::vector<double> ibis;
  for (size_t i = 1; i < beatT.size(); i++) if (beatT[i] > tEnd - 60 && beatT[i] < tEnd) ibis.push_back(beatT[i] - beatT[i-1]);
  double m = 0; for (double x : ibis) m += x; m /= ibis.size();
  double hrTrue = 60.0 / m;
  double sd2 = 0; for (size_t i = 1; i < ibis.size(); i++) sd2 += (ibis[i]-ibis[i-1])*(ibis[i]-ibis[i-1]);
  s.rmssdTrue = 1000.0 * sqrt(sd2 / (ibis.size() - 1));
  s.rmssdEst = r.rmssd;
  s.hrErr = r.hr1m - hrTrue;
  // true RR over last 300 s from phase
  double ph1 = brPhase[(size_t)((T - 300) * fs)], ph2 = brPhase[(size_t)(T * fs) - 1];
  double rrTrue = (ph2 - ph1) / (2*M_PI) / 300.0 * 60.0;   // breaths per minute incl. pauses
  s.rrErr = r.rr5m - rrTrue;
  // beat matching (after the motion episode end + 5 s to the end, excluding posture change)
  int tp = 0, fn = 0, fp = 0;
  std::vector<double> det;
  for (uint32_t i = eng.beats.oldest(); i < eng.beats.head; i++) det.push_back(tStart + eng.beats.at(i).n / 200.0);
  auto inEval = [&](double t) { return (t > 20 && t < motA) || (t > motB + 5 && t < posA) || (t > posB + 5 && t < T - 1); };
  std::vector<bool> used(det.size(), false);
  double latSum = 0; int latN = 0;
  for (double bt : beatT) {
    if (!inEval(bt)) continue;
    int best = -1; double bd = 1e9;
    for (size_t j = 0; j < det.size(); j++) { double d = det[j] - bt; if (fabs(d - 0.02) < 0.10 && !used[j] && fabs(d) < bd) { bd = fabs(d); best = (int)j; } }
    if (best >= 0) { used[best] = true; tp++; latSum += det[best] - bt; latN++; } else fn++;
  }
  for (size_t j = 0; j < det.size(); j++) if (inEval(det[j]) && !used[j]) { fp++;
    if (getenv("DBG") && fp < 40) { const BeatEvent& b = eng.beats.at(eng.beats.oldest()+j); printf("    FP at %.3f s ibi=%d fl=%d amp=%d\n", det[j], b.ibiMs, b.flags, b.amp); } }
  if (getenv("DBG")) for (size_t j = 0; j < det.size(); j++) { if (det[j] > 424 && det[j] < 480) { const BeatEvent& b = eng.beats.at(eng.beats.oldest()+j); printf("    beat %.3f ibi=%d fl=%d amp=%d %s\n", det[j], b.ibiMs, b.flags, b.amp, used[j]?"":"FP"); } }
  s.sens = 100.0 * tp / (tp + fn); s.ppv = 100.0 * tp / (tp + fp);
  for (uint32_t i = eng.events.oldest(); i < eng.events.head; i++) {
    const VEvent& e = eng.events.at(i);
    if (e.type == EV_PAUSE) { s.pauses++; double ts = tStart + e.tMs / 1000.0; if (ts < holdA - 3 || ts > holdB + 3) s.falsePauses++; if (verbose) printf("  pause event at %.1f s (dur %.1f s)\n", tStart + e.tMs/1000.0, e.durMs/1000.0); }
    if (e.type == EV_MOTION) { s.motionEv++; if (verbose) printf("  motion event at %.1f s (dur %.1f s)\n", tStart + e.tMs/1000.0, e.durMs/1000.0); }
    if (e.type == EV_POSTURE) { s.postureEv++; if (verbose) printf("  posture event at %.1f s\n", tStart + e.tMs/1000.0); }
  }
  if (verbose) printf("  beat timing latency (det - true) mean %.1f ms; HR true %.1f est %.1f; RR true %.2f est %.2f; RMSSD true %.1f est %.1f; pattern 0x%04x\n",
                      latN ? 1000*latSum/latN : 0, hrTrue, r.hr1m, rrTrue, r.rr5m, s.rmssdTrue, s.rmssdEst, r.pattern);
  s.rrCov = covN ? 100.0 * rrOk / covN : 0; s.hrCov = covN ? 100.0 * hrOk / covN : 0;
  bool expectPause = (holdB - holdA) >= ageProfileFor(sc.age).apneaSec;
  s.ok = fabs(s.hrErr) < 2.0 && fabs(s.rrErr) < 1.0 && s.sens > 95 && s.ppv > 95 && (expectPause ? s.pauses >= 1 : s.pauses == 0) && s.falsePauses == 0 && s.motionEv >= 1 && s.rrCov >= 90 && s.hrCov >= 90;
  return s;
}

#ifndef SIM_NO_MAIN
int main(int argc, char** argv) {
  std::vector<Scenario> list = {
    {"Adult, chest front, supine",      35, 68, 14, 12, 1.5, 1.2, 11, false},
    {"Adult, chest front, sitting",     35, 72, 16, 12, 1.5, 0.8, 12, true},
    {"Adult, side of ribcage (weak)",   50, 64, 13,  5, 0.6, 0.6, 13, true},
    {"Adult, back (very weak SCG)",     70, 58, 12,  3, 0.4, 0.5, 14, false},
    {"Child 4 y",                        4, 110, 24,  8, 1.0, 0.8, 15, false},
    {"Infant 6 months",                 0.5, 135, 40,  6, 0.8, 0.7, 16, false},
    {"Adult tachycardia",               30, 118, 22, 12, 1.5, 1.0, 17, true},
    {"Clone MPU: Z offset +0.2 g, warm-up drift", 35, 70, 15, 10, 1.2, 1.0, 18, false, {0.03, -0.04, 0.20}, 0.04},
    {"Sitting, then lying: breathing 75 % shallower", 35, 66, 14, 10, 1.2, 1.6, 19, true, {0, 0, 0}, 0, 200, 0.25},
    {"Breathing rotation about vertical axis", 35, 64, 13, 10, 1.2, 0.8, 20, false, {0, 0, 0}, 0, 1e9, 1, 2},
    {"Your case: clone MPU (+0.22 g Z) + lying, 65 % shallower", 45, 72, 15, 7, 0.9, 1.0, 21, true, {0.02, -0.03, 0.22}, 0.03, 300, 0.35},
  };
  int pass = 0;
  for (size_t i = 0; i < list.size(); i++) {
    printf("=== %s (HR %.0f, RR %.0f) ===\n", list[i].name, list[i].hr, list[i].rr);
    Score s = runScenario(list[i], true);
    printf("  RESULT: HR err %+.2f bpm | RR err %+.2f br/min | beat sens %.1f%% PPV %.1f%% | RMSSD %.1f vs %.1f ms | pauses %d (false %d) motion %d posture %d | avail HR %.0f%% RR %.0f%% -> %s\n\n",
           s.hrErr, s.rrErr, s.sens, s.ppv, s.rmssdTrue, s.rmssdEst, s.pauses, s.falsePauses, s.motionEv, s.postureEv, s.hrCov, s.rrCov, s.ok ? "PASS" : "CHECK");
    if (s.ok) pass++;
  }
  // negative control: device on a table, no person (no SCG, no breathing)
  {
    Scenario nc = {"No person (table)", 35, 70, 15, 0.0, 0.0, 0.0, 99, false};
    printf("=== %s ===\n", nc.name);
    Score s = runScenario(nc, true);
    (void)s;
  }
  // orientation sweep: 12 random mountings of the adult case
  printf("=== Orientation sweep (12 random sensor mountings) ===\n");
  int opass = 0;
  for (int k = 0; k < 12; k++) {
    Scenario sc = list[0]; sc.seed = 100 + k; sc.bodyUpright = (k & 1);
    Score s = runScenario(sc, false);
    printf("  mount %2d: HR err %+5.2f  RR err %+5.2f  sens %5.1f  ppv %5.1f  RR avail %3.0f%%  %s\n", k, s.hrErr, s.rrErr, s.sens, s.ppv, s.rrCov, s.ok ? "PASS" : "CHECK");
    if (s.ok) opass++;
  }
  printf("\nScenarios passed: %d/%zu, orientation sweep passed: %d/12\n", pass, list.size(), opass);
  return 0;
}
#endif
