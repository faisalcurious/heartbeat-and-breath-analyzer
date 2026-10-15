"use strict";
/* ===================== CardioResp analytics (rules + tiny ML) =====================
   JavaScript port of CardioRespMonitor/Analytics.h  - keep both in sync (testdata/xcheck_analytics.py proves it).
   Input  : status object S (keys as streamed by the device) and the age profile P
   Output : {sev, head, cond, score, mlc, mln, rel, agree, p[], ev[[lvl,text]], miss[], code, obs, need, news}
   v1.3: long-term minute record; illnesses are only named after enough calm monitoring.
*/
const ANA_CLASS = ["normal", "tachycardia", "bradycardia", "irregular rhythm", "fast / laboured breathing", "apnoea or slow breathing", "coughing"];
const ANA_ZN = ["normal", "mildly", "moderately", "severely"];
const ANA_LT_N = 240, ANA_NEED_INFECT = 30, ANA_NEED_APNOEA = 30, ANA_NEED_AF = 5, ANA_NEED_CSR = 10, ANA_NEED_SIRS = 20;
const ANA_PF_PERIODIC = 0x0200;
function anaNum(v) { return typeof v === "number" && isFinite(v); }
function anaClip(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }
function anaZone(v, lo, hi) {
  if (v > hi) return v <= hi * 1.2 ? 1 : v <= hi * 1.5 ? 2 : 3;
  if (v < lo) return v >= lo * 0.83 ? -1 : v >= lo * 0.667 ? -2 : -3;
  return 0;
}

/* S -> inputs used by the analytics (nulls become NaN, as in the firmware) */
function anaInputs(S) {
  const n = k => anaNum(S[k]) ? S[k] : NaN;
  return {
    hr:n("hr"), rr:n("rr"), nn:n("nn"), rmssd:n("rmssd"), sdnn:n("sdnn"), pnn50:n("pnn50"), hrmin:n("hrmin"), hrmax:n("hrmax"),
    hra:n("hra"), hrs:n("hrs"), hrb:n("hrb"), sp:anaNum(S.sp) ? S.sp : 0, pcv:n("pcv"), ampcv:n("ampcv"),
    apl:anaNum(S.apl) ? S.apl : 0, apc:anaNum(S.apc) ? S.apc : 0, amp1:n("amp1"), amp:n("amp"), ie:n("ie"), rrs:n("rrs"), rr1:n("rr1"),
    vhf:n("vhf"), mf:anaNum(S.mf) ? S.mf : 0, tbody:n("tbody"),
    ap5:S.ap5 | 0, cgh:S.cgh | 0, sqh:S.sqh | 0, sqr:S.sqr | 0, sig:S.sig | 0, v60:S.v60 | 0, b60:S.b60 | 0,
    pres:!!S.pres, mot:!!S.mot, post:!!S.post, win:anaNum(S.win) ? S.win : 0,
    rrn:n("rrn"), ctot:anaNum(S.ctot) ? S.ctot : 0, art:!!S.art, tcal:!!S.tcal, pf:S.pf | 0
  };
}

function anaFeatures(s, P) {
  const x = new Array(21).fill(0);
  const hrOk = s.pres && anaNum(s.hr), rrOk = anaNum(s.rr);
  const hmid = 0.5 * (P.hr[0] + P.hr[1]), hhalf = 0.5 * (P.hr[1] - P.hr[0]);
  const rmid = 0.5 * (P.rr[0] + P.rr[1]), rhalf = 0.5 * (P.rr[1] - P.rr[0]);
  if (hrOk) { x[0] = anaClip((s.hr - hmid) / hhalf, -4, 4) / 2; x[2] = 1; }
  if (rrOk) { x[1] = anaClip((s.rr - rmid) / rhalf, -4, 4) / 2; x[3] = 1; }
  if (hrOk && anaNum(s.nn) && s.nn > 200) {
    if (anaNum(s.rmssd)) x[4] = anaClip(s.rmssd / s.nn, 0, 0.4) * 5;
    if (anaNum(s.sdnn)) x[5] = anaClip(s.sdnn / s.nn, 0, 0.4) * 5;
  }
  if (hrOk && anaNum(s.pnn50)) x[6] = anaClip(s.pnn50, 0, 100) / 50;
  if (hrOk && anaNum(s.hrmin) && anaNum(s.hrmax) && s.hr > 0) x[7] = anaClip((s.hrmax - s.hrmin) / s.hr, 0, 1) * 3;
  if (hrOk && s.hr > 0) {
    let d = 0;
    for (const k of ["hra", "hrs", "hrb"]) if (anaNum(s[k])) d = Math.max(d, Math.abs(s[k] - s.hr));
    x[8] = anaClip(d / s.hr, 0, 0.5) * 4;
  }
  if (hrOk) x[9] = anaClip(s.sp, 0, 12) / 6;
  if (rrOk && anaNum(s.pcv)) x[10] = anaClip(s.pcv, 0, 1) * 3;
  if (rrOk && anaNum(s.ampcv)) x[11] = anaClip(s.ampcv, 0, 1.5) * 2;
  x[12] = anaClip(s.ap5, 0, 6) / 3;
  if (P.apnea > 0) x[13] = anaClip(Math.max(s.apl, s.apc) / P.apnea, 0, 2);
  x[14] = 1;
  if (rrOk && anaNum(s.amp1) && anaNum(s.amp) && s.amp > 1e-6) x[14] = anaClip(s.amp1 / s.amp, 0, 2);
  x[15] = 0.8;
  if (rrOk && anaNum(s.ie)) x[15] = anaClip(s.ie, 0, 2);
  if (rrOk && s.rr > 0 && anaNum(s.rrs)) x[16] = anaClip(Math.abs(s.rrs - s.rr) / s.rr, 0, 0.6) * 3;
  x[17] = anaClip(s.cgh, 0, 10) / 5;
  if (anaNum(s.vhf)) x[18] = anaClip(s.vhf, 0, 1) * 2;
  x[19] = anaClip(s.mf, 0, 1) * 2;
  x[20] = (s.sqh + s.sqr) / 200;
  return x;
}

function anaMl(x, M) {
  const h = M.W1.map((row, j) => { let a = M.b1[j]; for (let i = 0; i < row.length; i++) a += row[i] * x[i]; return a > 0 ? a : 0; });
  const z = M.W2.map((row, k) => { let a = M.b2[k]; for (let j = 0; j < row.length; j++) a += row[j] * h[j]; return a; });
  const m = Math.max(...z);
  const e = z.map(v => Math.exp(v - m));
  const s = e.reduce((a, b) => a + b, 0);
  return e.map(v => v / s);
}

/* per-second conditions shared by the tracker and the rules */
function anaHrOk(s) { return s.pres && anaNum(s.hr); }
function anaIrregNow(s) {
  if (!anaHrOk(s) || s.art || s.mot) return false;
  let dis = 0;
  for (const k of ["hra", "hrs", "hrb"]) if (anaNum(s[k])) dis = Math.max(dis, Math.abs(s[k] - s.hr));
  dis = s.hr > 0 ? dis / s.hr : 0;
  const rel = (anaNum(s.rmssd) && anaNum(s.nn) && s.nn > 200) ? s.rmssd / s.nn : 0;
  return s.sp < 2.2 && dis > 0.08 && rel > 0.10;
}
function anaStressNow(s, P) {
  if (!anaHrOk(s) || !anaNum(s.rr) || s.sqh < 40 || s.sqr < 40 || s.mot || s.art) return false;
  const adult = P.id >= 6;
  const hl = adult ? 90 : P.hr[1], rl = adult ? 20 : P.rr[1];
  return s.hr > hl && s.rr > rl;
}
function anaFeverNow(s) { return s.tcal && anaNum(s.tbody) && s.tbody >= 37.8; }
const ANA_NEWS_RR = rr => rr <= 8 ? 3 : rr <= 11 ? 1 : rr <= 20 ? 0 : rr <= 24 ? 2 : 3;
const ANA_NEWS_HR = hr => hr <= 40 ? 3 : hr <= 50 ? 1 : hr <= 90 ? 0 : hr <= 110 ? 1 : hr <= 130 ? 2 : 3;
const ANA_NEWS_T = t => t <= 35.0 ? 3 : t <= 36.0 ? 1 : t <= 38.0 ? 0 : t <= 39.0 ? 1 : 2;

/* history for event rules and the v1.3 minute record; call update(s, P, nowMs) once per status message (~1 Hz) */
class AnaTracker {
  constructor() { this.reset(); }
  reset() {
    this.goodHeartMs = 0; this.heartGoodSince = 0; this.lostSince = 0; this.lastMotionMs = 0; this.t0 = 0;
    this.hrFast = NaN; this.hrSlow = NaN; this.rrFast = NaN; this.rrSlow = NaN; this.wasGood = false; this.wasPost = false;
    this.lastMs = 0; this.curIdx = 0; this.acc = [0, 0, 0, 0, 0, 0, 0]; this.curCough = 0; this.curPause = 0;
    this.lastCtot = -1; this.inPause = false; this.hist = new Array(ANA_LT_N).fill(null); this.histN = 0;
  }
  closeMinute() {
    const sec = this.acc.map(a => Math.min(60, Math.floor((a + 500) / 1000)));
    this.hist[this.histN % ANA_LT_N] = {valid:sec[0], fever:sec[1], tachyp:sec[2], tachy:sec[3], irreg:sec[4], periodic:sec[5], stress:sec[6],
      coughs:Math.min(255, this.curCough), pauses:Math.min(255, this.curPause)};
    this.acc = [0, 0, 0, 0, 0, 0, 0]; this.curCough = 0; this.curPause = 0;
    this.histN++;
  }
  update(s, P, nowMs) {
    if (!this.t0) this.t0 = nowMs || 1;
    if (s.mot || s.art || (s.post && !this.wasPost)) this.lastMotionMs = nowMs;
    this.wasPost = s.post;
    const good = s.pres && anaNum(s.hr) && s.sqh >= 40 && !s.art;
    if (good) {
      if (!this.wasGood && (!this.goodHeartMs || nowMs - this.goodHeartMs > 5000)) this.heartGoodSince = nowMs;
      this.goodHeartMs = nowMs; this.lostSince = 0;
    } else if (this.wasGood || this.lostSince === 0) {
      if (this.lostSince === 0 && this.goodHeartMs) this.lostSince = this.goodHeartMs;
    }
    this.wasGood = good;
    if (good) {
      this.hrFast = anaNum(this.hrFast) ? 0.9 * this.hrFast + 0.1 * s.hr : s.hr;
      this.hrSlow = anaNum(this.hrSlow) ? 0.995 * this.hrSlow + 0.005 * s.hr : s.hr;
    }
    if (anaNum(s.rr) && s.sqr >= 30) {
      this.rrFast = anaNum(this.rrFast) ? 0.9 * this.rrFast + 0.1 * s.rr : s.rr;
      this.rrSlow = anaNum(this.rrSlow) ? 0.995 * this.rrSlow + 0.005 * s.rr : s.rr;
    }
    const idx = Math.floor((nowMs - this.t0) / 60000);
    while (this.curIdx < idx) { this.closeMinute(); this.curIdx++; if (idx - this.curIdx > ANA_LT_N) this.curIdx = idx - ANA_LT_N; }
    let dt = this.lastMs ? nowMs - this.lastMs : 1000;
    if (dt > 3000) dt = 3000;
    this.lastMs = nowMs;
    const hrQ = good, rrQ = anaNum(s.rr) && s.sqr >= 40;
    if ((hrQ || rrQ) && !s.mot) this.acc[0] += dt;
    if (anaFeverNow(s)) this.acc[1] += dt;
    if (rrQ && s.rr > P.rr[1]) this.acc[2] += dt;
    if (hrQ && s.hr > P.hr[1]) this.acc[3] += dt;
    if (anaIrregNow(s)) this.acc[4] += dt;
    if ((s.pf & ANA_PF_PERIODIC) && rrQ) this.acc[5] += dt;
    if (anaStressNow(s, P)) this.acc[6] += dt;
    if (s.ctot >= 0) {
      if (this.lastCtot >= 0 && s.ctot >= this.lastCtot) this.curCough += s.ctot - this.lastCtot;
      this.lastCtot = s.ctot;
    }
    const pauseNow = P.apnea > 0 && s.apc >= P.apnea;
    if (pauseNow && !this.inPause) this.curPause++;
    this.inPause = pauseNow;
  }
  window(W) {
    const a = {n:0, obs:0, coughs:0, coughBlocks:0, fever:0, tachyp:0, tachy:0, irreg:0, periodic:0, stress:0, pauses:0};
    const have = Math.min(this.histN, ANA_LT_N), n = Math.min(W, have);
    a.n = n;
    let blk = 0, blkKey = -1;
    for (let k = 0; k < n; k++) {
      const mi = this.histN - 1 - k, m = this.hist[mi % ANA_LT_N];
      if (m.valid >= 30) a.obs++;
      a.coughs += m.coughs; a.pauses += m.pauses;
      if (m.fever >= 30) a.fever++;
      if (m.tachyp >= 30) a.tachyp++;
      if (m.tachy >= 30) a.tachy++;
      if (m.irreg >= 40) a.irreg++;
      if (m.periodic >= 30) a.periodic++;
      if (m.stress >= 45) a.stress++;
      const key = Math.floor(mi / 5);
      if (key !== blkKey) { if (blk >= 2) a.coughBlocks++; blk = 0; blkKey = key; }
      blk += m.coughs;
    }
    if (blk >= 2) a.coughBlocks++;
    return a;
  }
}

function fx(v, d) { return Number(v).toFixed(d); }          // printf("%.Nf")
function f0(v) {            // printf("%.0f"): exact halves round to even, like the firmware
  v = Number(v); const fl = Math.floor(v), d = v - fl;
  const r = d === 0.5 ? (fl % 2 === 0 ? fl : fl + 1) : Math.round(v);
  return String(r === 0 ? 0 : r);
}

function anaEvaluate(T, s, P, nowMs, M) {
  const v = {sev:0, head:"", cond:"", score:0, mlc:0, mln:"", rel:0, agree:true, p:[], ev:[], miss:[], code:"ok", obs:0, need:ANA_NEED_INFECT, news:-1};
  const best = {sev:-1, pri:-1, code:"", head:"", cond:"",
    offer(sv, pri, code, head, cond) { if (sv > this.sev || (sv === this.sev && pri > this.pri)) { this.sev = sv; this.pri = pri; this.code = code; this.head = head; this.cond = cond; } }};
  const ev = (l, t) => { if (v.ev.length < 8) v.ev.push([l, t.slice(0, 139)]); };
  const miss = t => { if (v.miss.length < 4) v.miss.push(t.slice(0, 139)); };
  const hrOk = s.pres && anaNum(s.hr), rrOk = anaNum(s.rr);
  const [hrLo, hrHi] = P.hr, [rrLo, rrHi] = P.rr;
  const runMs = T.t0 ? nowMs - T.t0 : 0;
  const L60 = T.window(60), L20 = T.window(20), L10 = T.window(10);
  v.obs = L60.obs;

  if (hrOk && !s.art) {
    const z = anaZone(s.hr, hrLo, hrHi);
    if (z > 0) {
      best.offer(z, 40 + z, "tachy", `Heart rate ${ANA_ZN[z]} above range`, `Fast heart rate (${f0(s.hr)} bpm)`);
      ev(z >= 2 ? "a" : "w", `Heart rate ${f0(s.hr)} bpm is ${ANA_ZN[z]} above the age range ${f0(hrLo)}-${f0(hrHi)} bpm.`);
    } else if (z < 0) {
      best.offer(-z, 40 - z, "brady", `Heart rate ${ANA_ZN[-z]} below range`, `Slow heart rate (${f0(s.hr)} bpm)`);
      ev(-z >= 2 ? "a" : "w", `Heart rate ${f0(s.hr)} bpm is ${ANA_ZN[-z]} below the age range ${f0(hrLo)}-${f0(hrHi)} bpm.`);
    } else ev("o", `Heart rate ${f0(s.hr)} bpm is inside the age range ${f0(hrLo)}-${f0(hrHi)} bpm.`);
  }
  if (s.art) ev("i", "Strong breathing movement shakes the chest: heart rate paused until breathing calms (not used by any rule meanwhile).");
  if (rrOk) {
    const z = anaZone(s.rr, rrLo, rrHi);
    if (z > 0) {
      best.offer(z, 50 + z, "tachyp", `Breathing rate ${ANA_ZN[z]} above range`, `Fast breathing (${fx(s.rr, 1)} /min)`);
      ev(z >= 2 ? "a" : "w", `Breathing rate ${fx(s.rr, 1)} /min is ${ANA_ZN[z]} above the age range ${f0(rrLo)}-${f0(rrHi)} /min.`);
    } else if (z < 0) {
      best.offer(-z, 50 - z, "bradyp", `Breathing rate ${ANA_ZN[-z]} below range`, `Slow breathing (${fx(s.rr, 1)} /min)`);
      ev(-z >= 2 ? "a" : "w", `Breathing rate ${fx(s.rr, 1)} /min is ${ANA_ZN[-z]} below the age range ${f0(rrLo)}-${f0(rrHi)} /min.`);
    } else ev("o", `Breathing rate ${fx(s.rr, 1)} /min is inside the age range ${f0(rrLo)}-${f0(rrHi)} /min.`);
  }

  // spectral rules
  if (hrOk && !s.art) {
    let dis = 0;
    for (const k of ["hra", "hrs", "hrb"]) if (anaNum(s[k])) dis = Math.max(dis, Math.abs(s[k] - s.hr));
    dis = s.hr > 0 ? dis / s.hr : 0;
    const rel = (anaNum(s.rmssd) && anaNum(s.nn) && s.nn > 200) ? s.rmssd / s.nn : 0;
    if (anaIrregNow(s)) {
      if (L10.irreg >= ANA_NEED_AF && L10.obs >= ANA_NEED_AF) {
        best.offer(2, 62, "irreg", "Irregular heart rhythm (lasting)", `Irregular heart rhythm for ${L10.irreg} min: possible atrial fibrillation or frequent extra beats - an ECG is needed`);
        ev("a", `Rhythm irregular for ${L10.irreg} of the last ${L10.n} minutes (spectrum prominence ${fx(s.sp, 1)}, estimators differ ${f0(dis * 100)} %, RMSSD ${f0(rel * 100)} % of interval).`);
      } else {
        best.offer(1, 61, "irreg", "Irregular heartbeat noted", "Irregular heartbeat: the monitor is checking whether it lasts (5 minutes) before suggesting a cause");
        ev("w", `No clear beat-spectrum peak (${fx(s.sp, 1)}), estimators differ by ${f0(dis * 100)} %, RMSSD ${f0(rel * 100)} % of the interval: irregular beats now.`);
      }
    } else if (dis > 0.15) {
      ev("i", `Heart-rate estimators disagree by ${f0(dis * 100)} %; treat the value with caution.`);
    }
  }
  if (rrOk) {
    const rdis = (s.rr > 0 && anaNum(s.rrs)) ? Math.abs(s.rrs - s.rr) / s.rr : 0;
    if (anaNum(s.pcv) && s.pcv > 0.35 && rdis > 0.25) {
      best.offer(1, 30, "irrb", "Irregular breathing pattern", "Irregular breathing timing (spectrum and breath counting disagree)");
      ev("w", `Breath timing varies by ${f0(s.pcv * 100)} % and the spectral rate differs by ${f0(rdis * 100)} % from the counted rate.`);
    }
    if (anaNum(s.amp1) && anaNum(s.amp) && s.amp > 0 && s.amp1 < 0.5 * s.amp && s.rr > rrHi) {
      best.offer(2, 55, "labour", "Fast, shallow breathing", "Fast and shallow breathing (sign of breathing difficulty)");
      ev("a", `Breathing is fast (${fx(s.rr, 1)} /min) and the last minute is only ${f0(100 * s.amp1 / s.amp)} % as deep as the average.`);
    }
  }
  if (s.cgh >= 3) {
    best.offer(1, 45, "cough", s.cgh >= 6 ? "Frequent coughing" : "Coughing", `Coughing: ${s.cgh} cough bursts in the last 30 s`);
    ev("w", `${s.cgh} cough bursts in the last 30 s (explosive chest vibration, not movement).`);
  }

  // event rules
  if (s.apc >= P.apnea && P.apnea > 0) {
    best.offer(3, 90, "apnea", `Breathing pause ${f0(s.apc)} s`, "No breathing detected (breathing pause in progress)");
    ev("a", `No breath detected for ${f0(s.apc)} s (alarm threshold ${f0(P.apnea)} s for this age).`);
  } else if (s.ap5 > 0) {
    const sv = (s.ap5 >= 3 || s.apl >= 2 * P.apnea) ? 2 : 1;
    best.offer(sv, 52, "pauses", `${s.ap5} breathing pause${s.ap5 > 1 ? "s" : ""} in 5 min`, sv === 2 ? "Repeated breathing pauses: watch the person closely" : "A breathing pause was detected");
    ev(sv === 2 ? "a" : "w", `${s.ap5} pause${s.ap5 > 1 ? "s" : ""} of ${f0(P.apnea)} s or longer in 5 min (longest ${f0(s.apl)} s).`);
  }
  {
    const sinceGood = T.goodHeartMs ? nowMs - T.goodHeartMs : 0;
    const wasGoodLong = T.goodHeartMs && T.heartGoodSince && (T.goodHeartMs - T.heartGoodSince) >= 30000;
    const still = !s.mot && !s.art && (nowMs - T.lastMotionMs) > 8000 && s.mf < 0.35;
    if (!hrOk && wasGoodLong && still && sinceGood >= 10000 && sinceGood <= 600000 && T.lastMotionMs < T.goodHeartMs) {
      const noBreath = s.apc >= 6 || !rrOk;
      best.offer(3, noBreath ? 100 : 95, "arrest", `Heart signal lost for ${Math.floor(sinceGood / 1000)} s`,
        noBreath ? "Possible cardiac arrest: no heartbeat and no breathing detected" : "Possible cardiac arrest or asystole: heartbeat vibration lost while still");
      ev("a", `A good heart signal (${f0((T.goodHeartMs - T.heartGoodSince) / 1000)} s long) disappeared ${Math.floor(sinceGood / 1000)} s ago without any movement; sensor contact is unchanged${noBreath ? " and no breathing is detected" : ""}. Check responsiveness and pulse now.`);
    }
  }
  if (hrOk && !s.art && s.hr < hrLo * 0.5 && s.sqh >= 40) best.offer(3, 92, "brady3", "Very slow heart rate", "Very slow heart rate: possible impending arrest - check the person now");
  if (anaNum(T.hrFast) && anaNum(T.hrSlow) && anaNum(T.rrFast) && anaNum(T.rrSlow) && runMs > 600000) {
    const dh = T.hrFast - T.hrSlow, dr = T.rrFast - T.rrSlow;
    if (dh > 0.12 * T.hrSlow && dr > 3.0) {
      best.offer(1, 35, "trend", "Early warning: heart and breathing rates rising", "Heart and breathing rates rising together (early sign of deterioration)");
      ev("w", `Heart rate is up ${f0(dh)} bpm and breathing rate up ${fx(dr, 1)} /min compared with the last few minutes.`);
    }
  }
  if (anaFeverNow(s)) ev("w", `Body temperature ${fx(s.tbody, 1)} degC (${fx(s.tbody * 9 / 5 + 32, 1)} degF) is raised (sensor calibrated against a thermometer).`);
  else if (anaNum(s.tbody) && s.tbody >= 37.8 && !s.tcal) ev("i", `The sensor temperature reads ${fx(s.tbody, 1)} degC (${fx(s.tbody * 9 / 5 + 32, 1)} degF), but it is not calibrated against a thermometer (Settings): not used as fever.`);

  // v1.3 long-term patterns
  {
    const coughing = L60.coughBlocks >= 3 && L60.coughs >= 10;
    if (coughing && L60.obs >= ANA_NEED_INFECT && (L60.fever >= 10 || L60.tachyp * 2 >= L60.obs)) {
      best.offer(2, 60, "resp-inf", "Chest infection pattern", "Pattern compatible with a chest infection (for example flu, COVID-19 or pneumonia) - a test and a doctor's review are needed");
      ev("w", `Over ${L60.obs} calm minutes: ${L60.coughs} coughs in ${L60.coughBlocks} separate 5-minute periods, ${L60.fever >= 10 ? "raised temperature" : ""}${(L60.fever >= 10 && L60.tachyp * 2 >= L60.obs) ? " and " : ""}${L60.tachyp * 2 >= L60.obs ? "fast breathing" : ""}.`);
    } else if (coughing) {
      ev("i", `Coughing keeps returning (${L60.coughs} coughs). Infection patterns need ${ANA_NEED_INFECT} calm minutes with fever or fast breathing (now ${L60.obs}).`);
    }
    if (L60.obs >= ANA_NEED_APNOEA && L60.pauses > 0) {
      const idx = 60 * L60.pauses / L60.obs;
      if (idx >= 5) {
        const sv = idx >= 15 ? 2 : 1;
        best.offer(sv, 53, "osa", `${f0(idx)} breathing pauses per hour`, idx >= 30 ? "Pattern compatible with severe sleep apnoea - a sleep study and a doctor's review are needed"
          : idx >= 15 ? "Pattern compatible with moderate sleep apnoea - a sleep study is advised" : "Pattern compatible with mild sleep apnoea - discuss with a doctor");
        ev(sv === 2 ? "a" : "w", `${L60.pauses} breathing pauses of ${f0(P.apnea)} s or longer in ${L60.obs} calm minutes (${f0(idx)} per hour).`);
      }
    }
    if (L20.periodic >= ANA_NEED_CSR) {
      best.offer(2, 54, "csr", "Periodic breathing (lasting)", "Periodic (Cheyne-Stokes-like) breathing: seen with heart failure, stroke or at high altitude - needs medical review");
      ev("a", `Breathing has waxed and waned with pauses for ${L20.periodic} of the last ${L20.n} minutes.`);
    }
    if (L20.stress >= 15 && L20.obs >= 15 && T.histN >= ANA_NEED_SIRS) {
      best.offer(2, 59, "sirs", "Fast heart and breathing (lasting)", `Fast heart and breathing at rest for ${L20.stress} min${L20.fever >= 5 ? " with fever" : ""}: signs of body-wide stress (e.g. sepsis, dehydration) - see a doctor`);
      ev("a", `Heart rate and breathing rate have both been high for ${L20.stress} of the last ${L20.n} minutes while resting.`);
    }
  }
  if (P.id >= 6 && hrOk && !s.art && rrOk && runMs >= 300000) {
    const tOk = s.tcal && anaNum(s.tbody);
    const a = ANA_NEWS_RR(s.rr), b = ANA_NEWS_HR(s.hr), t = tOk ? ANA_NEWS_T(s.tbody) : 0;
    v.news = a + b + t;
    const red = a === 3 || b === 3 || t === 3;
    const risk = v.news >= 7 ? "high" : (v.news >= 5 || red) ? "medium" : "low";
    ev(v.news >= 5 || red ? "w" : "i", `Early warning score NEWS2 (partial: breathing, pulse${tOk ? ", temperature" : ""}): ${v.news} - ${risk} clinical risk.`);
    if (v.news >= 7) best.offer(2, 57, "news", "High early-warning score", "High early-warning score (NEWS2 partial >= 7): urgent clinical review advised");
  }
  if (hrOk && !s.art && P.rmssd > 0 && anaNum(s.rmssd) && s.rmssd < P.rmssd)
    ev("i", `RMSSD ${f0(s.rmssd)} ms is below about ${f0(P.rmssd)} ms (reduced vagal tone; trend indicator only).`);

  // ML
  const x = anaFeatures(s, P);
  v.p = anaMl(x, M);
  let top = 0;
  for (let k = 1; k < v.p.length; k++) if (v.p[k] > v.p[top]) top = k;
  v.mlc = top; v.mln = ANA_CLASS[top]; v.score = v.p[top];

  // fusion
  let sev = best.sev < 0 ? 0 : best.sev;
  if (best.sev < 0) {
    if (hrOk || rrOk) {
      if (top !== 0 && v.score >= 0.80 && top !== 6) {
        sev = 1;
        best.offer(1, 10, "ml", `Pattern check: ${ANA_CLASS[top]}`, `Model suspects ${ANA_CLASS[top]} (rules found no zone breach)`);
      } else best.offer(0, 0, "ok", "No abnormality detected", "Heart and breathing pattern within the expected range");
    } else best.offer(0, 0, "none", "Waiting for a usable signal", "No heart or breathing signal yet");
  }
  v.sev = Math.min(3, sev); v.head = best.head.slice(0, 139); v.cond = best.cond.slice(0, 139); v.code = best.code;
  {
    let ruleCls = 0;
    const rc = best.code;
    if (rc === "tachy") ruleCls = 1; else if (rc === "brady" || rc === "brady3") ruleCls = 2;
    else if (rc === "irreg") ruleCls = 3; else if (rc === "tachyp" || rc === "labour") ruleCls = 4;
    else if (rc === "apnea" || rc === "pauses" || rc === "bradyp" || rc === "osa" || rc === "csr") ruleCls = 5;
    else if (rc === "cough" || rc === "resp-inf") ruleCls = 6;
    else if (rc === "arrest" || rc === "sirs" || rc === "news" || rc === "trend") ruleCls = -1;
    v.agree = ruleCls < 0 || ruleCls === top || (sev === 0 && top === 0) || v.score < 0.6;
    if (sev >= 1 && ruleCls >= 0 && ruleCls !== top && v.score >= 0.6) ev("i", `The learned model leans towards "${ANA_CLASS[top]}" (${f0(v.score * 100)} %) while the rules found "${best.cond}".`);
    else if (sev === 0 && top !== 0 && v.score >= 0.6) ev("i", `The learned model leans towards "${ANA_CLASS[top]}" (${f0(v.score * 100)} %), below the alert level.`);
  }
  // reliability
  {
    let q = (hrOk && rrOk) ? 0.5 * (s.sqh + s.sqr) : hrOk ? 0.8 * s.sqh : rrOk ? 0.8 * s.sqr : 0;
    q *= 1 - 0.7 * anaClip(s.mf, 0, 1);
    if (hrOk) {
      let dis = 0;
      if (anaNum(s.hra)) dis = Math.max(dis, Math.abs(s.hra - s.hr));
      if (anaNum(s.hrs)) dis = Math.max(dis, Math.abs(s.hrs - s.hr));
      if (s.hr > 0) q *= 1 - anaClip(dis / s.hr, 0, 0.3);
    }
    if (s.mot) q *= 0.5;
    if (s.art) q *= 0.6;
    v.rel = Math.round(anaClip(q, 0, 100));
  }
  if (!hrOk) miss("No reliable heart signal: heart rate, rhythm and spectral rules were skipped.");
  else if (!anaNum(s.rmssd)) miss("Heart-rate variability needs about 1 minute of still recording (20+ beats).");
  if (!rrOk) miss("No reliable breathing signal: breathing rules were skipped.");
  else if (s.win < 150) miss(`Breathing window is only ${f0(s.win)} s of the 5 min used; the rate may still shift.`);
  if (L60.obs < ANA_NEED_INFECT) miss(`Long-term pattern checks (infection, sleep apnoea, lasting irregular rhythm) need ${ANA_NEED_INFECT} calm minutes: ${L60.obs} so far.`);
  else if (!anaNum(s.tbody) || !s.tcal) miss("Body temperature is not calibrated (Settings): fever is not used.");
  if (s.mf > 0.2) miss(`Movement covered ${f0(s.mf * 100)} % of the last minute; those parts were excluded.`);
  if (!v.ev.length) ev("i", "Not enough valid data to list evidence yet.");
  return v;
}

if (typeof module !== "undefined") module.exports = {anaInputs, anaFeatures, anaMl, anaEvaluate, AnaTracker, ANA_CLASS};
