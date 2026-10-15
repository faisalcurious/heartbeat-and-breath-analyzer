// Runs the body simulator through the real engine and records the exact
// WebSocket stream (Protocol.h) and the /api/log JSON for dashboard tests.
#define SIM_NO_MAIN
#include "sim_test.cpp"
#include "Protocol.h"
#include <string>

struct StrWriter {
  std::string s;
  void add(const char* x) { s += x; }
  void fmt(const char* f, ...) { char t[512]; va_list ap; va_start(ap, f); vsnprintf(t, sizeof(t), f, ap); va_end(ap); s += t; }
};

int main(int argc, char** argv) {
  const char* outDir = argc > 1 ? argv[1] : ".";
  std::string p1 = std::string(outDir) + "/stream.jsonl", p2 = std::string(outDir) + "/log.json";
  FILE* fo = fopen(p1.c_str(), "w");
  static char buf[8192];
  StreamCursor cur;
  double nextW = 0, nextF = 0;
  g_simT = 660.0;
  g_hook = [&](double t, VitalsEngine& e, bool an) {
    double tm = t - 8.5;
    if (tm >= nextW) {
      nextW = tm + 0.05;
      VitalsResults r; e.getResults(r);
      JBuf j(buf, sizeof(buf));
      if (protoWave(j, e, cur, r.heartPresent)) fprintf(fo, "{\"ts\":%.3f,\"m\":%s}\n", tm, buf);
    }
    if (an) {
      VitalsResults r; e.getResults(r);
      JBuf j(buf, sizeof(buf));
      j.raw("{\"t\":\"s\""); j.ki("sid", e.session()); j.ki("up", (long)(t * 1000)); j.ki("cal", 2); j.kf("calp", 1, 2); j.ki("cala", 1);
      j.ks("calmsg", "Calibrated: |g| = 1.012 g, tilt reference stored"); j.kf("age", 35, 2);
      protoResults(j, r);
      j.raw(",\"bat\":{\"ok\":1,\"v\":3.94,\"p\":68}"); j.kf("temp", 31.4f, 1); j.ki("heap", 142000); j.ki("minheap", 128000);
      j.ki("cli", 1); j.ki("ovf", 0); j.ki("i2c", 0); j.ki("au", 38); j.ki("aumax", 96); j.raw("}");
      fprintf(fo, "{\"ts\":%.3f,\"m\":%s}\n", tm, buf);
    }
    if (tm >= nextF) {
      nextF = tm + 2.0;
      static VitalsSpectra sp; e.getSpectra(sp);
      JBuf j(buf, sizeof(buf));
      protoSpectra(j, sp);
      fprintf(fo, "{\"ts\":%.3f,\"m\":%s}\n", tm, buf);
    }
  };
  Scenario sc = {"Adult, chest front, supine", 35, 68, 14, 12, 1.5, 1.2, 11, false};
  // capture the engine at the end for the log
  VitalsEngine* last = nullptr;
  auto inner = g_hook;
  g_hook = [&](double t, VitalsEngine& e, bool an) { inner(t, e, an); last = &e; if (t >= g_simT - 0.006) {
      StrWriter w; char scratch[4096];
      LogMeta m = {"CardioResp-A1B2C3", "1.0.0", "chest-worn 6-axis motion sensor", 35.0f};
      protoLog(w, e, m, scratch, sizeof(scratch));
      FILE* fl = fopen(p2.c_str(), "w"); fputs(w.s.c_str(), fl); fclose(fl);
      fprintf(stderr, "log.json %zu bytes\n", w.s.size());
    } };
  Score s = runScenario(sc, false);
  fclose(fo);
  fprintf(stderr, "HR err %.2f RR err %.2f sens %.1f ppv %.1f\n", s.hrErr, s.rrErr, s.sens, s.ppv);
  (void)last;
  return 0;
}
