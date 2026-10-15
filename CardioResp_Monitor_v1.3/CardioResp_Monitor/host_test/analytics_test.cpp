// analytics_test.cpp - runs Analytics.h on a text file of status snapshots and prints one JSON verdict per line.
// Used by testdata/xcheck_analytics.py to prove the C++ and JavaScript ports agree.
// build: g++ -O2 -std=c++17 -Ihost_test/stubs -ICardioRespMonitor host_test/analytics_test.cpp -o analytics_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Analytics.h"
#include "Protocol.h"

static void setf(float& d, const char* v) { d = (strcmp(v, "nan") == 0) ? NAN : strtof(v, nullptr); }
int main(int argc, char** argv) {
  if (argc < 2) return 1;
  FILE* f = fopen(argv[1], "r");
  if (!f) return 2;
  static char line[2048];
  static AnaTracker T;
  while (fgets(line, sizeof line, f)) {
    AnaIn s; float age = 30; uint32_t t = 0; bool reset = false;
    for (char* tok = strtok(line, " \r\n"); tok; tok = strtok(nullptr, " \r\n")) {
      char* eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char* k = tok; const char* v = eq + 1;
      if (!strcmp(k, "age")) age = strtof(v, nullptr); else if (!strcmp(k, "t")) t = (uint32_t)strtoul(v, nullptr, 10);
      else if (!strcmp(k, "reset")) reset = atoi(v) != 0;
      else if (!strcmp(k, "hr")) setf(s.hr, v); else if (!strcmp(k, "rr")) setf(s.rr, v); else if (!strcmp(k, "nn")) setf(s.nn, v);
      else if (!strcmp(k, "rmssd")) setf(s.rmssd, v); else if (!strcmp(k, "sdnn")) setf(s.sdnn, v); else if (!strcmp(k, "pnn50")) setf(s.pnn50, v);
      else if (!strcmp(k, "hrmin")) setf(s.hrmin, v); else if (!strcmp(k, "hrmax")) setf(s.hrmax, v);
      else if (!strcmp(k, "hra")) setf(s.hra, v); else if (!strcmp(k, "hrs")) setf(s.hrs, v); else if (!strcmp(k, "hrb")) setf(s.hrb, v);
      else if (!strcmp(k, "sp")) setf(s.sp, v); else if (!strcmp(k, "pcv")) setf(s.pcv, v); else if (!strcmp(k, "ampcv")) setf(s.ampcv, v);
      else if (!strcmp(k, "apl")) setf(s.apl, v); else if (!strcmp(k, "apc")) setf(s.apc, v); else if (!strcmp(k, "amp1")) setf(s.amp1, v);
      else if (!strcmp(k, "amp")) setf(s.amp, v); else if (!strcmp(k, "ie")) setf(s.ie, v); else if (!strcmp(k, "rrs")) setf(s.rrs, v);
      else if (!strcmp(k, "rr1")) setf(s.rr1, v); else if (!strcmp(k, "vhf")) setf(s.vhf, v); else if (!strcmp(k, "mf")) setf(s.mf, v);
      else if (!strcmp(k, "tbody")) setf(s.tbody, v); else if (!strcmp(k, "win")) setf(s.win, v);
      else if (!strcmp(k, "ap5")) s.ap5 = atoi(v); else if (!strcmp(k, "cgh")) s.cgh = atoi(v); else if (!strcmp(k, "sqh")) s.sqh = atoi(v);
      else if (!strcmp(k, "sqr")) s.sqr = atoi(v); else if (!strcmp(k, "pres")) s.pres = atoi(v) != 0; else if (!strcmp(k, "mot")) s.mot = atoi(v) != 0;
      else if (!strcmp(k, "post")) s.post = atoi(v) != 0;
      else if (!strcmp(k, "rrn")) setf(s.rrn, v); else if (!strcmp(k, "ctot")) s.ctot = atol(v);
      else if (!strcmp(k, "art")) s.art = atoi(v) != 0; else if (!strcmp(k, "tcal")) s.tcal = atoi(v) != 0;
      else if (!strcmp(k, "pf")) s.pf = atoi(v);
    }
    if (reset) T.reset();
    T.update(s, ageProfileFor(age), t);
    static Verdict V; anaEvaluate(T, s, ageProfileFor(age), t, V);
    static char buf[4096]; JBuf j(buf, sizeof buf); anaWriteJson(j, V);
    puts(buf);
  }
  return 0;
}
