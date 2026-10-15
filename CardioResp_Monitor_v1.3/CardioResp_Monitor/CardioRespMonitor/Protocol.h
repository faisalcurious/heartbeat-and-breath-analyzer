// =====================================================================
//  Protocol.h  -  JSON messages between the device and the dashboard
//  Portable (no Arduino dependencies) so the PC simulator produces the
//  exact same byte stream as the firmware.
//
//  WebSocket  server -> page
//   "w" every 50 ms : new waveform samples + detected beats / breaths
//   "s" every 1 s   : all results (see protoResults for keys)
//   "f" every 2 s   : spectra
//   "i" on connect  : device / sensor / calibration info (firmware)
//  WebSocket  page -> server : {"cmd":"age","v":years} | recal | reset |
//                              polarity | info
//  HTTP GET /api/log : last 10 minutes for the report (protoLog)
// =====================================================================
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "VitalsEngine.h"

struct JBuf {
  char* b; size_t cap; size_t len = 0;
  JBuf(char* buf, size_t c) : b(buf), cap(c) { b[0] = 0; }
  void raw(const char* s) { while (*s && len + 1 < cap) b[len++] = *s++; b[len] = 0; }
  void fmt(const char* f, ...) {
    if (len + 1 >= cap) return;
    va_list ap; va_start(ap, f);
    int n = vsnprintf(b + len, cap - len, f, ap);
    va_end(ap);
    if (n > 0) len = (len + (size_t)n < cap) ? len + n : cap - 1;
  }
  void num(float v, int dec) {
    if (isnan(v) || isinf(v)) { raw("null"); return; }
    fmt("%.*f", dec, (double)v);
  }
  void key(const char* k) { if (len && b[len - 1] != '{' && b[len - 1] != '[') raw(","); raw("\""); raw(k); raw("\":"); }
  void kf(const char* k, float v, int dec) { key(k); num(v, dec); }
  void ki(const char* k, long v) { key(k); fmt("%ld", v); }
  void ks(const char* k, const char* v) {
    key(k); raw("\"");
    for (const char* p = v; *p; p++) {
      if (*p == '"' || *p == '\\') raw("\\");
      if ((unsigned char)*p < 0x20) continue;
      char c[2] = {*p, 0}; raw(c);
    }
    raw("\"");
  }
  void kv3(const char* k, const float* v, int dec) { key(k); raw("["); for (int i = 0; i < 3; i++) { if (i) raw(","); num(v[i], dec); } raw("]"); }
  bool nearlyFull(size_t reserve) const { return len + reserve >= cap; }
};

struct StreamCursor { uint32_t sid = 0xFFFFFFFFu, disp = 0, resp = 0, beat = 0, breath = 0; };

// (Re)position the stream cursors: on a new session or a new client, resend
// a short history so the page can draw immediately (the page de-duplicates
// by absolute sample index).
inline void protoSync(StreamCursor& c, VitalsEngine& e, bool force) {
  if (!force && e.session() == c.sid) return;
  c.sid = e.session();
  uint32_t dh = e.disp.head, rh = e.resp.head, bh = e.beats.head, brh = e.breaths.head;
  c.disp = dh > 300 ? dh - 300 : 0;
  c.resp = rh > 600 ? rh - 600 : 0;
  c.beat = bh > 8 ? bh - 8 : 0;
  c.breath = brh > 20 ? brh - 20 : 0;
}

// "w" message. Returns false when there is nothing new to send.
inline bool protoWave(JBuf& j, VitalsEngine& e, StreamCursor& c, bool heartPresent) {
  protoSync(c, e, false);
  if (e.disp.head < c.disp || e.resp.head < c.resp || e.beats.head < c.beat || e.breaths.head < c.breath) {
    protoSync(c, e, true);           // rings were restarted under us
    return false;
  }
  uint32_t dh = e.disp.head;
  if (c.disp + VE_DISP_LEN - 64 < dh) c.disp = dh - (VE_DISP_LEN - 64);
  uint32_t dEnd = dh;
  if (dEnd - c.disp > 150) dEnd = c.disp + 150;
  uint32_t rh = e.resp.head;
  if (c.resp + VE_RESP_LEN - 64 < rh) c.resp = rh - (VE_RESP_LEN - 64);
  uint32_t rEnd = rh;
  if (rEnd - c.resp > 300) rEnd = c.resp + 300;
  if (dEnd == c.disp && rEnd == c.resp && e.beats.head == c.beat && e.breaths.head == c.breath) return false;

  j.raw("{\"t\":\"w\"");
  j.ki("sid", (long)c.sid);
  j.ki("i", (long)c.disp);
  j.key("a"); j.raw("[");
  for (uint32_t k = c.disp; k < dEnd; k++) { if (k != c.disp) j.raw(","); j.fmt("%d", e.disp.at(k).scg); }
  j.raw("]");
  j.key("g"); j.raw("[");
  for (uint32_t k = c.disp; k < dEnd; k++) { if (k != c.disp) j.raw(","); j.fmt("%d", e.disp.at(k).gcg); }
  j.raw("]");
  j.key("e"); j.raw("[");
  for (uint32_t k = c.disp; k < dEnd; k++) { if (k != c.disp) j.raw(","); j.fmt("%d", e.disp.at(k).env); }
  j.raw("]");
  j.ki("ri", (long)c.resp);
  j.key("r"); j.raw("[");
  uint32_t k = c.resp;
  for (; k < rEnd && !j.nearlyFull(700); k++) {
    if (k != c.resp) j.raw(",");
    j.fmt("%d", e.resp.at(k));
    if (e.respMotionAt(k)) j.raw(".5");        // fractional .5 marks movement
  }
  rEnd = k;
  j.raw("]");
  uint32_t bh = e.beats.head;
  if (c.beat < e.beats.oldest()) c.beat = e.beats.oldest();
  j.key("b"); j.raw("[");
  bool first = true;
  uint32_t kb = c.beat;
  for (; kb < bh && !j.nearlyFull(300); kb++) {
    (void)heartPresent;                        // v1.2: markers are always sent; the page draws unconfirmed ones muted
    const BeatEvent& b = e.beats.at(kb);
    if (!first) j.raw(",");
    first = false;
    j.fmt("[%lu,%u,%u]", (unsigned long)(b.n / VE_DISP_DIV), (unsigned)b.ibiMs, (unsigned)b.flags);
  }
  j.raw("]");
  uint32_t brh = e.breaths.head;
  if (c.breath < e.breaths.oldest()) c.breath = e.breaths.oldest();
  j.key("br"); j.raw("[");
  uint32_t kr = c.breath;
  for (; kr < brh && !j.nearlyFull(200); kr++) {
    const BreathEvent& b = e.breaths.at(kr);
    if (kr != c.breath) j.raw(",");
    unsigned long te10 = b.teMs / (1000 / (VE_FS / VE_RESP_DIV));
    unsigned long pk = b.n10 > te10 ? b.n10 - te10 : 0;
    j.fmt("[%lu,%u,%u,%u,%d,%u,%lu]", (unsigned long)b.n10, (unsigned)b.periodMs, (unsigned)b.tiMs, (unsigned)b.teMs, (int)b.ampMdeg, (unsigned)b.flags, pk);
  }
  j.raw("]");
  j.ki("m", e.motionNow() ? 1 : 0);
  j.raw("}");
  c.disp = dEnd; c.resp = rEnd; c.beat = kb; c.breath = kr;
  return true;
}

inline void protoResults(JBuf& j, const VitalsResults& r) {
  j.kf("hr", r.hr1m, 1); j.kf("hri", r.hrInst, 1); j.kf("hra", r.hrAcf, 1); j.kf("hrs", r.hrSpec, 1); j.kf("hrb", r.hrBeats, 1);
  j.ki("sqh", r.sqiH); j.ki("pres", r.heartPresent ? 1 : 0);
  j.kf("nn", r.meanNN, 0); j.kf("sdnn", r.sdnn, 1); j.kf("rmssd", r.rmssd, 1); j.kf("pnn50", r.pnn50, 1);
  j.kf("hrmin", r.hrMin, 1); j.kf("hrmax", r.hrMax, 1); j.ki("b60", r.beats60); j.ki("v60", r.valid60);
  j.kf("cov", r.coverage, 2); j.kf("qa", r.qa, 2); j.kf("qg", r.qg, 2); j.kf("qf", r.qf, 2); j.kf("wa", r.wa, 2); j.kf("wg", r.wg, 2);
  j.kf("snr", r.pulseSnr, 2); j.kf("sp", r.specProm, 1);
  j.kf("rr", r.rr5m, 1); j.kf("rr1", r.rr1m, 1); j.kf("rrs", r.rrSpec, 1); j.ki("sqr", r.sqiR);
  j.kf("amp", r.ampMean, 3); j.kf("amp1", r.amp1m, 3); j.kf("ampcv", r.ampCV, 3); j.kf("pcv", r.periodCV, 3);
  j.kf("ti", r.tiMean, 2); j.kf("te", r.teMean, 2); j.kf("ie", r.ieRatio, 2);
  j.ki("n5", r.breaths5); j.ki("ap5", r.pauses5); j.kf("apl", r.longestPause5, 1); j.kf("apc", r.currentPause, 1);
  j.ki("sig", r.sighs5); j.kf("win", r.windowSec, 0); j.ki("pol", r.polarity); j.kf("rsp", r.rrSpecProm, 1);
  j.kv3("g", r.g, 3); j.kf("roll", r.roll, 1); j.kf("pitch", r.pitch, 1); j.kf("tilt", r.tiltFromCal, 1);
  j.ki("post", r.postureChanged ? 1 : 0); j.ki("mot", r.motion ? 1 : 0); j.kf("mf", r.motionFrac60, 2);
  // v1.3: fast breathing rate, breath / cough counts, breathing artefact on the heart channel
  j.kf("rrn", r.rrNow, 1); j.ki("b1m", r.b1m); j.ki("btot", (long)r.breathTotal); j.ki("ctot", (long)r.coughTotal);
  j.ki("art", r.hrArtefact ? 1 : 0); j.kf("brart", r.brArt, 2);
  j.kv3("vc", r.vCard, 3); j.kv3("vg", r.vGyro, 3); j.kv3("vr", r.vResp, 3);
  j.kv3("ab", r.accBias, 3);
  j.ki("pf", r.pattern); j.kf("fs", r.fsActual, 2); j.ki("gap", (long)r.gapSamples); j.kv3("bias", r.gyroBias, 3);
  j.ki("ag", r.ageGroup); j.ki("now", (long)r.nowMs);
  // v1.2: absolute chest-vibration values (never masked; the page colours them) and cough bursts
  j.kf("vra", r.vibRmsA, 2); j.kf("vrg", r.vibRmsG, 3); j.kf("vpk", r.vibPkA, 1); j.kf("vhf", r.vibHf, 2); j.ki("cgh", r.cough30);
}

// Expired / missing licence: heart rate and breathing rate only (no waveforms, spectra, rules or ML).
// v1.3: sensor placement and gravity are always included (protoOrientation), with or without a licence.
inline void protoResultsLite(JBuf& j, const VitalsResults& r) {
  j.kf("hr", r.hr1m, 1); j.kf("hri", r.hrInst, 1); j.ki("sqh", r.sqiH); j.ki("pres", r.heartPresent ? 1 : 0);
  j.kf("rr", r.rr5m, 1); j.kf("rr1", r.rr1m, 1); j.kf("rrn", r.rrNow, 1); j.ki("b1m", r.b1m); j.ki("sqr", r.sqiR); j.kf("win", r.windowSec, 0);
  j.ki("pf", r.pattern & (PF_TACHYCARDIA | PF_BRADYCARDIA | PF_TACHYPNEA | PF_BRADYPNEA));
  j.kf("apc", r.currentPause, 1);                    // v1.2: breathing-pause safety alarm also works without a licence
  j.ki("art", r.hrArtefact ? 1 : 0);                 // (movement "mot" is sent by protoOrientation)
  j.ki("ag", r.ageGroup); j.ki("now", (long)r.nowMs);
}

// v1.3: sensor placement and gravity, sent in EVERY status message (also before Start and without a licence).
// g = unit gravity vector in the sensor axes, roll / pitch in degrees, tilt = change since the last calibration,
// mot = moving now, still = 0..100 how still the sensor is, src = 1 engine (measuring) / 2 preview (not measuring)
struct OrientInfo { float g[3]; float roll, pitch, tilt; bool mot, post; int still; int src; float mf; };
inline void protoOrientation(JBuf& j, const OrientInfo& o) {
  j.kv3("g", o.g, 3); j.kf("roll", o.roll, 1); j.kf("pitch", o.pitch, 1); j.kf("tilt", o.tilt, 1);
  j.ki("post", o.post ? 1 : 0); j.ki("mot", o.mot ? 1 : 0); j.kf("mf", o.mf, 2); j.ki("still", o.still); j.ki("osrc", o.src);
}

inline void protoSpectra(JBuf& j, const VitalsSpectra& s) {
  j.raw("{\"t\":\"f\"");
  j.kf("cd", s.cardDf, 5); j.kf("cp", s.cardPeakHz, 4);
  j.key("c"); j.raw("["); for (int i = 0; i < VE_SPEC_CARD_N; i++) { if (i) j.raw(","); j.fmt("%u", (unsigned)s.card[i]); } j.raw("]");
  j.kf("rd", s.respDf, 5); j.kf("rp", s.respPeakHz, 4);
  j.key("r"); j.raw("["); for (int i = 0; i < VE_SPEC_RESP_N; i++) { if (i) j.raw(","); j.fmt("%u", (unsigned)s.resp[i]); } j.raw("]");
  j.kf("vd", s.vibDf, 4);
  j.key("va"); j.raw("["); for (int i = 0; i < VE_SPEC_VIB_N; i++) { if (i) j.raw(","); j.fmt("%d", (int)s.vibA[i]); } j.raw("]");
  j.key("vg"); j.raw("["); for (int i = 0; i < VE_SPEC_VIB_N; i++) { if (i) j.raw(","); j.fmt("%d", (int)s.vibG[i]); } j.raw("]");
  j.raw("}");
}

struct LogMeta { const char* dev; const char* fw; const char* model; float age; };

// Streams the last 10 minutes. W must provide add(const char*) and fmt(...).
// Ring entries close to being overwritten are skipped (the sensor task keeps
// writing while this runs).
template <class W>
void protoLog(W& w, VitalsEngine& e, const LogMeta& m, char* scratch, size_t scratchLen) {
  const uint32_t nowMs = e.nowMs();
  const uint32_t fromMs = nowMs > 600000u ? nowMs - 600000u : 0;
  VitalsResults r; e.getResults(r);
  static VitalsSpectra sp; e.getSpectra(sp);
  w.fmt("{\"dev\":\"%s\",\"fw\":\"%s\",\"model\":\"%s\",\"sid\":%lu,\"now\":%lu,\"from\":%lu,\"fs\":%.3f,\"age\":%.2f,\"group\":\"%s\",",
        m.dev, m.fw, m.model, (unsigned long)e.session(), (unsigned long)nowMs, (unsigned long)fromMs,
        (double)r.fsActual, (double)m.age, ageProfileFor(m.age).name);
  {
    JBuf j(scratch, scratchLen);
    j.raw("\"res\":{");
    protoResults(j, r);
    j.raw("},");
    w.add(scratch);
  }
  w.add("\"trend\":[");
  bool first = true;
  for (uint32_t k = e.trend.oldest() + (e.trend.head > (uint32_t)VE_TREND_LEN ? 4 : 0); k < e.trend.head; k++) {
    const TrendPoint& t = e.trend.at(k);
    if (t.tMs < fromMs) continue;
    w.fmt("%s[%lu,%u,%u,%u,%u,%u,%u]", first ? "" : ",", (unsigned long)t.tMs, (unsigned)t.hr10, (unsigned)t.rr10, (unsigned)t.sqiH, (unsigned)t.sqiR, (unsigned)t.flags, (unsigned)t.tilt);
    first = false;
  }
  w.add("],\"beats\":[");
  first = true;
  for (uint32_t k = e.beats.oldest() + (e.beats.head > (uint32_t)VE_BEAT_LEN ? 16 : 0); k < e.beats.head; k++) {
    const BeatEvent& b = e.beats.at(k);
    uint32_t t = e.msOfSample(b.n);
    if (t < fromMs) continue;
    w.fmt("%s[%lu,%u,%u,%u]", first ? "" : ",", (unsigned long)t, (unsigned)b.ibiMs, (unsigned)b.flags, (unsigned)b.amp);
    first = false;
  }
  w.add("],\"breaths\":[");
  first = true;
  for (uint32_t k = e.breaths.oldest() + (e.breaths.head > (uint32_t)VE_BREATH_LEN ? 8 : 0); k < e.breaths.head; k++) {
    const BreathEvent& b = e.breaths.at(k);
    uint32_t t = e.msOfSample(b.n10 * VE_RESP_DIV);
    if (t < fromMs) continue;
    w.fmt("%s[%lu,%u,%u,%u,%d,%u]", first ? "" : ",", (unsigned long)t, (unsigned)b.periodMs, (unsigned)b.tiMs, (unsigned)b.teMs, (int)b.ampMdeg, (unsigned)b.flags);
    first = false;
  }
  w.add("],\"events\":[");
  first = true;
  for (uint32_t k = e.events.oldest(); k < e.events.head; k++) {
    const VEvent& ev = e.events.at(k);
    if (ev.tMs + ev.durMs < fromMs) continue;
    w.fmt("%s[%lu,%u,%lu]", first ? "" : ",", (unsigned long)ev.tMs, (unsigned)ev.type, (unsigned long)ev.durMs);
    first = false;
  }
  // breathing waveform (10 Hz, mdeg) + motion runs [start, length]
  const uint32_t rh = e.resp.head;
  const uint32_t rcount = rh < 6000u ? rh : 6000u;
  const uint32_t r0 = rh - rcount;
  w.fmt("],\"resp\":{\"t0\":%lu,\"fs\":10,\"d\":[", (unsigned long)e.msOfSample(r0 * VE_RESP_DIV));
  for (uint32_t k = r0; k < rh; k++) w.fmt(k == r0 ? "%d" : ",%d", (int)e.resp.at(k));
  w.add("],\"mr\":[");
  first = true;
  for (uint32_t k = r0; k < rh;) {
    if (!e.respMotionAt(k)) { k++; continue; }
    uint32_t s = k;
    while (k < rh && e.respMotionAt(k)) k++;
    w.fmt("%s[%lu,%lu]", first ? "" : ",", (unsigned long)(s - r0), (unsigned long)(k - s));
    first = false;
  }
  // 10-s SCG / GCG strip (100 Hz)
  const uint32_t dh = e.disp.head;
  const uint32_t dcount = dh < 1000u ? dh : 1000u;
  const uint32_t d0 = dh - dcount;
  w.fmt("]},\"strip\":{\"t0\":%lu,\"fs\":100,\"a\":[", (unsigned long)e.msOfSample(d0 * VE_DISP_DIV));
  for (uint32_t k = d0; k < dh; k++) w.fmt(k == d0 ? "%d" : ",%d", (int)e.disp.at(k).scg);
  w.add("],\"g\":[");
  for (uint32_t k = d0; k < dh; k++) w.fmt(k == d0 ? "%d" : ",%d", (int)e.disp.at(k).gcg);
  w.add("]},\"spec\":{");
  w.fmt("\"cd\":%.5f,\"rd\":%.5f,\"c\":[", (double)sp.cardDf, (double)sp.respDf);
  for (int i = 0; i < VE_SPEC_CARD_N; i++) w.fmt(i ? ",%u" : "%u", (unsigned)sp.card[i]);
  w.add("],\"r\":[");
  for (int i = 0; i < VE_SPEC_RESP_N; i++) w.fmt(i ? ",%u" : "%u", (unsigned)sp.resp[i]);
  w.add("]}}");
}
