// =====================================================================
//  sim_abnormal.cpp - end-to-end simulation of ABNORMAL situations:
//  body model (sim_test.cpp) -> VitalsEngine -> Analytics (rules + ML)
//  Checks that early-abnormality detection fires, and that a normal
//  subject does not raise alarms.
//  g++ -O2 -std=c++17 -I../CardioRespMonitor -Istubs sim_abnormal.cpp ../CardioRespMonitor/VitalsEngine.cpp -o sim_abn
// =====================================================================
#define SIM_NO_MAIN
#include "sim_test.cpp"
#include "Analytics.h"
#include <map>
#include <string>

struct Run { std::map<std::string, int> codes; std::map<std::string, double> first; int maxSev = 0; int n = 0, alarms = 0; double tFirstAlarm = -1; Verdict last; bool covidEarly = false; int maxCough30 = 0; bool coughEv = false; };

static Run runAna(const Scenario& sc, double T, double tAfter = 100.0, float tbody = NAN, bool tcal = false) {
  g_simT = T;
  Run R; AnaTracker tr; bool init = false;
  g_hook = [&](double t, VitalsEngine& eng, bool an) {
    if (!an || t < tAfter) return;
    VitalsResults r; eng.getResults(r);
    AnaIn in = anaFromResults(r, tbody, tcal);
    if (!init) { tr.reset(); init = true; }
    tr.update(in, ageProfileFor(sc.age), (uint32_t)(t * 1000));
    Verdict v; anaEvaluate(tr, in, ageProfileFor(sc.age), (uint32_t)(t * 1000), v);
    R.n++; R.codes[v.ruleCode]++; if (!R.first.count(v.ruleCode)) R.first[v.ruleCode] = t;
    if (t < tAfter + 1800 && (strstr(v.condition, "COVID") || strstr(v.headline, "infection"))) R.covidEarly = true;
    if (r.cough30 > R.maxCough30) R.maxCough30 = r.cough30;
    for (int e = 0; e < v.nEv; e++) if (strstr(v.ev[e].txt, "cough bursts")) R.coughEv = true;
    if (v.severity > R.maxSev) R.maxSev = v.severity;
    if (v.severity >= 2) { R.alarms++; if (R.tFirstAlarm < 0) R.tFirstAlarm = t; }
    R.last = v;
  };
  runScenario(sc, false);
  g_hook = nullptr;
  return R;
}
static void show(const char* name, const Run& R) {
  printf("=== %s ===\n  rule codes seen:", name);
  for (auto& kv : R.codes) printf(" %s x%d (first %.0f s)", kv.first.c_str(), kv.second, R.first.at(kv.first));
  printf("\n  max severity %d, last: [%s] %s | ML %s %.0f %% | reliability %d\n", R.maxSev, R.last.ruleCode, R.last.headline, ANA_CLASS[R.last.mlClass], R.last.mlScore * 100, R.last.reliability);
}
int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) { printf("  %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; };
  // 1) normal adult: no alarms (the scripted 18 s breath hold and movement are real events: pause alarm is allowed there)
  { Scenario sc = {"normal", 35, 68, 14, 12, 1.5, 1.2, 31, false}; Run R = runAna(sc, 330);
    show("Normal adult (330 s, no events after 100 s)", R);
    check(R.alarms == 0, "no warning/critical alarms for a normal adult");
    check(R.codes["ok"] >= 0.8 * R.n, ">= 80 % of verdicts 'No abnormality'"); }
  // 2) tachycardia
  { Scenario sc = {"tachy", 35, 135, 16, 12, 1.5, 1.0, 32, true}; Run R = runAna(sc, 330);
    show("Tachycardia 135 bpm", R); check(R.codes["tachy"] > 0 && R.maxSev >= 2, "tachycardia moderate or worse detected"); }
  // 3) bradycardia
  { Scenario sc = {"brady", 35, 44, 14, 12, 1.5, 1.0, 33, true}; Run R = runAna(sc, 330);
    show("Bradycardia 44 bpm", R); check(R.codes["brady"] > 0 && R.maxSev >= 2, "bradycardia moderate or worse detected"); }
  // 4) fast breathing
  { Scenario sc = {"tachyp", 35, 90, 32, 12, 1.5, 1.0, 34, true}; Run R = runAna(sc, 330);
    show("Fast breathing 32/min", R); check((R.codes["tachyp"] + R.codes["labour"]) > 0 && R.maxSev >= 2, "fast breathing detected"); }
  // 5) heart stops at t=250 s (breathing continues, sensor stays on, no movement)
  { Scenario sc = {"arrest", 35, 70, 14, 12, 1.5, 1.2, 35, false}; g_heartStop = 250; Run R = runAna(sc, 330, 100.0); g_heartStop = 1e9;
    show("Heart signal disappears at 250 s while still", R);
    check(R.codes["arrest"] > 0, "possible-cardiac-arrest rule fires");
    if (R.codes["arrest"] > 0) printf("  time from heart stop to alarm: %.0f s\n", R.first["arrest"] - 250.0); }
  // 6) cough bursts (6 within 30 s from t=200 s)
  { Scenario sc = {"cough", 35, 72, 15, 12, 1.5, 1.2, 36, false}; g_coughs = {200, 204, 208, 212, 216, 220}; Run R = runAna(sc, 300, 100.0); g_coughs.clear();
    show("Coughing: 6 bursts in 20 s", R); check(R.codes["cough"] > 0, "cough rule fires"); }
  // 7) cough + fast breathing + fever for only 5 minutes -> coughing is reported, but NO infection is named (v1.3)
  { Scenario sc = {"covid-like", 35, 95, 26, 12, 1.5, 1.0, 37, true}; g_coughs = {200, 204, 208, 212}; Run R = runAna(sc, 300, 100.0, 38.4f, true); g_coughs.clear();
    show("Cough + fast breathing + 38.4 degC for 5 min", R);
    check(R.maxCough30 >= 3 && R.coughEv, "coughing is reported in the evidence");
    check(R.codes["resp-inf"] == 0 && !R.covidEarly, "no infection / COVID named on 5 minutes of data"); }
  // 7b) the same over 36 minutes (coughs keep returning every ~2 min) -> chest-infection pattern only after 30 calm minutes
  { Scenario sc = {"chest infection", 35, 95, 26, 12, 1.5, 1.0, 39, true};
    g_coughs.clear(); for (double tc = 150; tc < 2200; tc += 115) { g_coughs.push_back(tc); g_coughs.push_back(tc + 5); }
    Run R = runAna(sc, 2280, 60.0, 38.4f, true); g_coughs.clear();
    show("Coughs every 2 min + fast breathing + 38.4 degC for 36 min", R);
    check(R.codes["resp-inf"] > 0, "chest-infection pattern reported after long monitoring");
    check(!R.covidEarly && (!R.first.count("resp-inf") || R.first["resp-inf"] >= 60 + 1800), "not before 30 calm minutes");
    if (R.first.count("resp-inf")) printf("  first reported at %.0f s of monitoring\n", R.first["resp-inf"] - 60.0); }
  // 5b) v1.3: the person changed posture (420-423 s, the posture flag stays on) and the heart stops at 480 s
  { Scenario sc = {"arrest after turning", 35, 70, 14, 12, 1.5, 1.2, 40, false}; g_heartStop = 480; Run R = runAna(sc, 560, 100.0); g_heartStop = 1e9;
    show("Posture change at 420 s, heart signal disappears at 480 s", R);
    check(R.codes["arrest"] > 0, "possible-cardiac-arrest rule still fires after a posture change");
    if (R.codes["arrest"] > 0) printf("  time from heart stop to alarm: %.0f s\n", R.first["arrest"] - 480.0); }
  // 8) breath hold 360-378 s (scenario script) -> apnoea alarm
  { Scenario sc = {"apnea", 35, 68, 14, 12, 1.5, 1.2, 38, false}; Run R = runAna(sc, 400, 100.0);
    show("Breath hold 18 s (360-378 s)", R); check(R.codes["apnea"] > 0 || R.codes["pauses"] > 0, "breathing pause detected"); }
  printf("\n%s (%d failed checks)\n", fails ? "SOME CHECKS FAILED" : "ALL ABNORMAL-SCENARIO CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
