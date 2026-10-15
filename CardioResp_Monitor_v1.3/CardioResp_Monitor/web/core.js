"use strict";
/* ===================== CardioResp core (pure functions) ===================== */
const PF = {TACHY:1, BRADY:2, IRREG:4, TACHYP:8, BRADYP:16, IRRB:32, PAUSES:64, SHALLOW:128, SIGHS:256, PERIODIC:512, LOWHRV:1024};
const EVN = {1:"Breathing pause", 2:"Movement", 3:"Posture change", 4:"Calibration / restart"};
// Mirrors AgeNorms.h (PALS / AHA reference ranges, awake resting values)
const AGE_TABLE = [
  {id:0, name:"Newborn (0-1 month)",      hr:[100,205], rr:[30,60], apnea:20, rmssd:0},
  {id:1, name:"Infant (1-12 months)",     hr:[100,180], rr:[30,53], apnea:20, rmssd:0},
  {id:2, name:"Toddler (1-2 years)",      hr:[98,140],  rr:[22,37], apnea:15, rmssd:0},
  {id:3, name:"Preschool (3-5 years)",    hr:[80,120],  rr:[20,28], apnea:15, rmssd:0},
  {id:4, name:"School age (6-11 years)",  hr:[75,118],  rr:[18,25], apnea:15, rmssd:0},
  {id:5, name:"Adolescent (12-17 years)", hr:[60,100],  rr:[12,20], apnea:10, rmssd:20},
  {id:6, name:"Adult (18-39 years)",      hr:[60,100],  rr:[12,20], apnea:10, rmssd:20},
  {id:7, name:"Adult (40-64 years)",      hr:[60,100],  rr:[12,20], apnea:10, rmssd:15},
  {id:8, name:"Older adult (65+ years)",  hr:[60,100],  rr:[12,20], apnea:10, rmssd:10}
];
function ageProfile(y) {
  const lim = [1/12, 1, 3, 6, 12, 18, 40, 65];
  let i = 0;
  while (i < lim.length && y >= lim[i]) i++;
  return AGE_TABLE[i];
}
function isNum(v) { return typeof v === "number" && isFinite(v); }
function f(v, d) { return isNum(v) ? v.toFixed(d) : "--"; }
function classify(v, r) { if (!isNum(v)) return null; return v < r[0] ? "low" : v > r[1] ? "high" : "normal"; }
function mean(a) { return a.length ? a.reduce((s, x) => s + x, 0) / a.length : NaN; }
function sd(a) { if (a.length < 2) return NaN; const m = mean(a); return Math.sqrt(a.reduce((s, x) => s + (x - m) * (x - m), 0) / a.length); }
function median(a) { if (!a.length) return NaN; const b = a.slice().sort((x, y) => x - y); const n = b.length >> 1; return b.length & 1 ? b[n] : 0.5 * (b[n - 1] + b[n]); }
function sqiWord(q) { return !isNum(q) ? "unknown" : q >= 80 ? "excellent" : q >= 60 ? "good" : q >= 40 ? "fair" : q >= 15 ? "poor" : "no usable signal"; }
function ageText(y) {
  if (!isNum(y)) return "--";
  if (y < 2) { const m = Math.round(y * 12); return m + (m === 1 ? " month" : " months"); }
  return (Math.round(y * 10) / 10) + " years";
}
function patternLabels(pf) {
  const o = [];
  if (pf & PF.TACHY) o.push("fast heart rate");
  if (pf & PF.BRADY) o.push("slow heart rate");
  if (pf & PF.IRREG) o.push("irregular rhythm");
  if (pf & PF.TACHYP) o.push("fast breathing");
  if (pf & PF.BRADYP) o.push("slow breathing");
  if (pf & PF.IRRB) o.push("irregular breathing");
  if (pf & PF.PAUSES) o.push("breathing pauses");
  if (pf & PF.SHALLOW) o.push("shallower breathing");
  if (pf & PF.SIGHS) o.push("frequent sighs");
  if (pf & PF.PERIODIC) o.push("periodic breathing");
  if (pf & PF.LOWHRV) o.push("low HRV");
  return o;
}
function axisUp(g) {
  if (!g) return "--";
  const n = ["X", "Y", "Z"];
  let k = 0;
  for (let i = 1; i < 3; i++) if (Math.abs(g[i]) > Math.abs(g[k])) k = i;
  return (g[k] >= 0 ? "+" : "-") + n[k] + " (" + Math.round(Math.abs(g[k]) * 100) + "% of gravity)";
}

/* Respiratory sinus arrhythmia: heart-rate swing inside each breath */
function computeRSA(beats, breaths) {
  // beats: [{t (ms), ibi, valid}], breaths: [{t (end, ms), per (ms), valid}]
  const sw = [];
  let j = 0;
  for (const br of breaths) {
    if (!br.valid) continue;
    const a = br.t - br.per, b = br.t;
    while (j < beats.length && beats[j].t < a) j++;
    let lo = Infinity, hi = -Infinity, n = 0;
    for (let k = j; k < beats.length && beats[k].t <= b; k++) {
      const x = beats[k];
      if (!x.valid || !(x.ibi > 0)) continue;
      const hr = 60000 / x.ibi;
      lo = Math.min(lo, hr); hi = Math.max(hi, hr); n++;
    }
    if (n >= 2) sw.push(hi - lo);
  }
  return sw.length >= 3 ? median(sw) : NaN;
}

/* ---------- live detailed analysis (from a status message) ---------- */
function liveAnalysis(S, P, X) {
  // S: status object, P: age profile, X: {rsa}
  const sec = [];
  const heart = [], hrv = [], br = [], pat = [], cp = [], q = [], ori = [];
  // v1.3: breathing is judged on the breaths counted in the last minute (newest breaths / 5-minute average as fall-backs)
  const rrRef = isNum(S.rr1) ? S.rr1 : isNum(S.rr) ? S.rr : isNum(S.rrn) ? S.rrn : NaN;
  const hrState = classify(S.hr, P.hr), rrState = classify(rrRef, P.rr);
  // ---- heart
  if (S.art) heart.push(["warn", "Strong breathing movement is shaking the chest, so the heart rate is paused instead of following the breathing. It comes back by itself when breathing calms; the values below may be up to a minute old."]);
  if (!S.pres || !isNum(S.hr)) {
    heart.push(["alert", "No reliable heart signal yet. Press the sensor firmly against the chest (the sternum gives the strongest heart vibration), keep still and wait up to one minute."]);
  } else {
    const rng = P.hr[0] + "-" + P.hr[1] + " bpm";
    if (hrState === "normal") heart.push(["ok", `Average heart rate ${f(S.hr, 0)} bpm over the last minute is within the resting range for ${P.name.toLowerCase()} (${rng}).`]);
    else if (hrState === "high") heart.push(["alert", `Average heart rate ${f(S.hr, 0)} bpm is above the resting range for this age (${rng}). This can follow activity, fever, pain, anxiety, dehydration or caffeine; a persistently raised resting rate should be reviewed.`]);
    else heart.push(["warn", `Average heart rate ${f(S.hr, 0)} bpm is below the resting range for this age (${rng}). This can be normal during sleep or in trained athletes; with dizziness or fainting it needs medical review.`]);
    const est = [S.hrb, S.hra, S.hrs].filter(isNum);
    if (est.length >= 2) {
      const spread = (Math.max(...est) - Math.min(...est)) / S.hr;
      heart.push([spread < 0.08 ? "ok" : "warn", `Independent estimates: beat-to-beat ${f(S.hrb, 1)}, autocorrelation ${f(S.hra, 1)} and spectrum ${f(S.hrs, 1)} bpm` +
        (spread < 0.08 ? " agree, which supports the reading." : " differ, so treat the value with caution (movement or weak contact).")]);
    }
    if (isNum(S.hrmin) && isNum(S.hrmax)) heart.push(["info", `Beat-to-beat range in the last minute: ${f(S.hrmin, 0)}-${f(S.hrmax, 0)} bpm from ${S.v60} accepted of ${S.b60} detected beats.`]);
    if (S.pf & PF.IRREG) heart.push(["alert", "Beat intervals are irregular beyond normal breathing-related variation. This may reflect extra beats or an arrhythmia such as atrial fibrillation, but can also be caused by movement. An ECG is needed to confirm."]);
  }
  sec.push({title:"Heart", items:heart});
  // ---- HRV
  if (isNum(S.rmssd)) {
    let lvl = "info", txt = `RMSSD ${f(S.rmssd, 0)} ms, SDNN ${f(S.sdnn, 0)} ms, pNN50 ${f(S.pnn50, 0)} %.`;
    if (P.rmssd > 0) {
      if (S.rmssd < P.rmssd) { lvl = "warn"; txt += ` RMSSD is below about ${P.rmssd} ms, which suggests reduced vagal (rest-and-digest) activity. Stress, poor sleep, illness, dehydration or age can lower it.`; }
      else { lvl = "ok"; txt += " Short-term variability is in the usual resting range for this age."; }
    } else txt += " Paediatric HRV depends strongly on age and state; compare with the child's own earlier recordings.";
    hrv.push([lvl, txt]);
    hrv.push(["info", "Values come from one minute of intervals measured from chest vibration; the clinical HRV standard uses 5-minute ECG recordings, so use them for trends."]);
  } else {
    hrv.push(["info", "Heart-rate variability needs at least one minute of still recording with 20 or more accepted beats."]);
  }
  sec.push({title:"Heart-rate variability", items:hrv});
  // ---- breathing
  if (!isNum(rrRef)) {
    br.push(["alert", "No reliable breathing signal yet. Keep the sensor on the ribcage or upper abdomen, stay still and breathe normally for at least 30 seconds."]);
  } else {
    const rng = P.rr[0] + "-" + P.rr[1] + " breaths/min";
    const how = isNum(S.rr1) ? `counted over the last minute${isNum(S.b1m) ? ` (${S.b1m} breaths)` : ""}` : `over the last ${Math.max(1, Math.round((S.win || 0) / 60))} min`;
    if (rrState === "normal") br.push(["ok", `Breathing rate ${f(rrRef, 1)} /min ${how} is within the resting range for this age (${rng}).`]);
    else if (rrState === "high") br.push(["alert", `Breathing rate ${f(rrRef, 1)} /min ${how} is above the resting range for this age (${rng}). Fast breathing can accompany fever, lung or heart problems, pain or anxiety.`]);
    else br.push(["warn", `Breathing rate ${f(rrRef, 1)} /min ${how} is below the resting range for this age (${rng}). Slow breathing occurs in deep sleep, relaxation exercises or with sedating medicines.`]);
    if (isNum(S.rrn) && Math.abs(S.rrn - rrRef) > 0.2 * rrRef) br.push(["info", `The newest breaths run at ${f(S.rrn, 1)} /min, so breathing is ${S.rrn > rrRef ? "speeding up" : "slowing down"}.`]);
    if (isNum(S.rr) && isNum(S.rr1) && Math.abs(S.rr1 - S.rr) > 0.2 * S.rr) br.push(["info", `The 5-minute average is ${f(S.rr, 1)} /min, so the rate has changed in the last few minutes.`]);
    if (isNum(S.ie)) {
      if (S.ie < 0.33) br.push(["warn", `Inspiration to expiration ratio 1:${f(1 / S.ie, 1)}: expiration is markedly prolonged. This pattern can be seen with airway narrowing (asthma, COPD).`]);
      else if (S.ie > 1.05) br.push(["warn", `Inspiration is longer than expiration (I:E 1:${f(1 / S.ie, 1)}), which is unusual at rest. If the trace looks upside down use "Flip breathing direction".`]);
      else br.push(["ok", `Inspiration ${f(S.ti, 1)} s, expiration ${f(S.te, 1)} s (I:E 1:${f(1 / S.ie, 1)}), a normal resting proportion.`]);
    }
  }
  sec.push({title:"Breathing", items:br});
  // ---- pattern
  if (isNum(S.pcv)) {
    const cv = S.pcv * 100;
    pat.push([cv < 20 ? "ok" : cv < 35 ? "info" : "warn", `Breath-to-breath timing varies by ${f(cv, 0)} % (` + (cv < 20 ? "regular" : cv < 35 ? "moderately variable" : "irregular") + `), depth varies by ${f((S.ampcv || 0) * 100, 0)} %.`]);
  }
  if (S.apc > 0) pat.push(["alert", `Breathing pause in progress: ${f(S.apc, 0)} s.`]);
  if (S.ap5 > 0) pat.push(["warn", `${S.ap5} breathing pause${S.ap5 > 1 ? "s" : ""} of ${P.apnea} s or longer in the last 5 minutes (longest ${f(S.apl, 0)} s).` + (S.pf & PF.PERIODIC ? " Pauses alternating with bigger breaths suggest periodic breathing." : "")]);
  else if (isNum(S.rr)) pat.push(["ok", `No breathing pauses of ${P.apnea} s or longer in the last 5 minutes.`]);
  if (S.pf & PF.SHALLOW) pat.push(["warn", "Breathing in the last minute is less than half as deep as the 5-minute average (shallow breathing, or the sensor loosened)."]);
  if (S.sig > 0) pat.push([S.sig >= 2 ? "info" : "ok", `${S.sig} sigh${S.sig > 1 ? "s" : ""} (breaths over twice the usual depth) in 5 minutes. Occasional sighs are normal; frequent sighing can accompany anxiety or breathlessness.`]);
  if (isNum(S.amp)) pat.push(["info", `Average chest-wall tilt per breath ${f(S.amp, 2)}° (relative depth; it depends on placement and body build).`]);
  if (isNum(S.ctot) && S.ctot > 0) pat.push(["info", `${S.ctot} cough${S.ctot > 1 ? "s" : ""} detected since the measurement started (short explosive chest bursts; body movements and taps are not counted).`]);
  if (pat.length) sec.push({title:"Breathing pattern", items:pat});
  // ---- coupling
  if (isNum(S.hr) && isNum(rrRef) && S.pres && !S.art) {
    const ratio = S.hr / rrRef;
    cp.push([ratio >= 3 && ratio <= 6.5 ? "ok" : "info", `Heart beats per breath: ${f(ratio, 1)} (typical 4-5 at rest in adults).`]);
    if (isNum(X && X.rsa)) {
      const r = X.rsa;
      cp.push([r >= 2 ? "ok" : "info", `Respiratory sinus arrhythmia: heart rate swings by about ${f(r, 1)} bpm within each breath` +
        (r >= 2 ? ", showing healthy breathing-linked vagal modulation." : "; a small swing is common with age, fast breathing or reduced vagal tone.")]);
    }
    sec.push({title:"Heart-breathing coupling", items:cp});
  }
  // ---- signal quality
  q.push([S.sqh >= 60 ? "ok" : S.sqh >= 30 ? "warn" : "alert", `Heart signal quality ${S.sqh}/100 (${sqiWord(S.sqh)}); breathing signal quality ${S.sqr}/100 (${sqiWord(S.sqr)}).`]);
  if (isNum(S.wa)) q.push(["info", `Heart detection currently uses ${Math.round(S.wa * 100)} % accelerometer (chest vibration) and ${Math.round((1 - S.wa) * 100)} % gyroscope (chest rotation), weighted by the periodicity of each (${f(S.qa, 2)} and ${f(S.qg, 2)}).`]);
  if (S.mf > 0.05) q.push(["warn", `Movement affected ${Math.round(S.mf * 100)} % of the last minute; those segments are excluded.`]);
  if (isNum(S.brart) && S.brart >= 0.32) q.push(["info", `The heartbeat channel follows the breathing movement (${Math.round(S.brart * 100)} % similarity); beats in that period are not trusted.`]);
  if (isNum(S.cov) && S.pres) q.push([S.cov > 0.8 ? "ok" : "info", `${Math.round(S.cov * 100)} % of the last minute is covered by accepted beat intervals.`]);
  sec.push({title:"Signal quality", items:q});
  // ---- orientation
  if (S.g) {
    ori.push(["info", `Sensor axis pointing up: ${axisUp(S.g)}. Roll ${f(S.roll, 0)}°, pitch ${f(S.pitch, 0)}°.`]);
    ori.push([S.post ? "warn" : "ok", `Orientation has changed by ${f(S.tilt, 0)}° since calibration` + (S.post ? ", indicating a posture change. Recalibrate if the person will stay in the new position." : ".")]);
    sec.push({title:"Orientation", items:ori});
  }
  return sec;
}

/* ---------------- 10-minute report from /api/log ---------------- */
function computeReport(L) {
  const P = ageProfile(L.age);
  const tr = L.trend || [];
  const t0 = tr.length ? Math.max(L.from, tr[0][0]) : L.from;
  const t1 = L.now;
  const R = {P, t0, t1, durMin:(t1 - t0) / 60000};
  // beats
  const beats = (L.beats || []).map(b => ({t:b[0], ibi:b[1], valid:!!(b[2] & 1), sb:!!(b[2] & 2), amp:b[3]}));
  const v = beats.filter(b => b.valid && b.ibi > 0);
  const ib = v.map(b => b.ibi);
  R.nBeats = beats.length; R.nValid = v.length;
  R.validPct = beats.length ? 100 * v.length / beats.length : NaN;
  R.hrMean = ib.length >= 5 ? 60000 / mean(ib) : NaN;
  R.sdnn = ib.length >= 20 ? sd(ib) : NaN;
  const d = [];
  for (let i = 1; i < beats.length; i++) if (beats[i].valid && beats[i - 1].valid && beats[i].ibi > 0 && beats[i - 1].ibi > 0) d.push(beats[i].ibi - beats[i - 1].ibi);
  R.rmssd = d.length >= 20 ? Math.sqrt(mean(d.map(x => x * x))) : NaN;
  R.pnn50 = d.length >= 20 ? 100 * d.filter(x => Math.abs(x) > 50).length / d.length : NaN;
  const sd1 = d.length >= 20 ? Math.sqrt(0.5) * sd(d) : NaN;
  R.sd1 = sd1; R.sd2 = isNum(R.sdnn) && isNum(sd1) ? Math.sqrt(Math.max(0, 2 * R.sdnn * R.sdnn - sd1 * sd1)) : NaN;
  // per-second trend (only good-quality seconds)
  const th = tr.filter(p => p[1] > 0 && p[3] >= 30).map(p => p[1] / 10);
  const trr = tr.filter(p => p[2] > 0 && p[4] >= 30).map(p => p[2] / 10);
  // robust min / max (5th / 95th percentile of per-second instantaneous rate)
  const pct = (a, p) => { if (!a.length) return NaN; const b = a.slice().sort((x, y) => x - y); return b[Math.min(b.length - 1, Math.max(0, Math.round(p * (b.length - 1))))]; };
  R.hrMin = pct(th, 0.05); R.hrMax = pct(th, 0.95);
  if (!isNum(R.hrMean) && th.length > 30) R.hrMean = mean(th);
  R.sqiH = mean(tr.map(p => p[3])); R.sqiR = mean(tr.map(p => p[4]));
  // breaths
  const brs = (L.breaths || []).map(b => ({t:b[0], per:b[1], ti:b[2], te:b[3], amp:b[4] / 1000, valid:!!(b[5] & 1), motion:!!(b[5] & 2), sigh:!!(b[5] & 4)}));
  const vb = brs.filter(b => b.valid);
  R.nBreaths = vb.length;
  const effSec = brs.filter(b => !b.motion).reduce((s, b) => s + b.per, 0) / 1000;
  R.rr = effSec >= 60 && vb.length >= 3 ? 60 * vb.length / effSec : vb.length >= 3 ? 60000 / mean(vb.map(b => b.per)) : NaN;
  R.rrMin = pct(trr, 0.05); R.rrMax = pct(trr, 0.95);
  R.perCV = vb.length >= 5 ? sd(vb.map(b => b.per)) / mean(vb.map(b => b.per)) : NaN;
  R.amp = vb.length ? mean(vb.map(b => b.amp)) : NaN;
  R.ampCV = vb.length >= 5 ? sd(vb.map(b => b.amp)) / R.amp : NaN;
  R.ti = vb.length ? mean(vb.map(b => b.ti)) / 1000 : NaN;
  R.te = vb.length ? mean(vb.map(b => b.te)) / 1000 : NaN;
  R.ie = isNum(R.ti) && R.te > 0 ? R.ti / R.te : NaN;
  R.sighs = vb.filter(b => b.sigh).length;
  // events
  const ev = (L.events || []).map(e => ({t:e[0], type:e[1], dur:e[2]}));
  R.events = ev.filter(e => e.t + e.dur >= t0 - 1000);
  R.pauses = R.events.filter(e => e.type === 1);
  R.longestPause = R.pauses.reduce((m, e) => Math.max(m, e.dur / 1000), 0);
  R.motionSec = R.events.filter(e => e.type === 2).reduce((s, e) => s + e.dur / 1000, 0);
  R.postureChanges = R.events.filter(e => e.type === 3).length;
  R.rsa = computeRSA(beats, brs.map(b => ({t:b.t, per:b.per, valid:b.valid})));
  R.hrPerBreath = isNum(R.hrMean) && isNum(R.rr) ? R.hrMean / R.rr : NaN;
  R.irregular = !!((L.res && L.res.pf) & PF.IRREG);
  R.hrState = classify(R.hrMean, P.hr); R.rrState = classify(R.rr, P.rr);
  R.findings = reportFindings(R);
  return R;
}

function reportFindings(R) {
  const P = R.P, o = [];
  if (!isNum(R.hrMean)) o.push(["alert", "Heart rate could not be measured reliably in this recording (poor sensor contact or movement)."]);
  else if (R.hrState === "normal") o.push(["ok", `Mean heart rate ${f(R.hrMean, 0)} bpm, within the resting range for ${P.name.toLowerCase()} (${P.hr[0]}-${P.hr[1]} bpm).`]);
  else o.push([R.hrState === "high" ? "alert" : "warn", `Mean heart rate ${f(R.hrMean, 0)} bpm is ${R.hrState === "high" ? "above" : "below"} the resting range for ${P.name.toLowerCase()} (${P.hr[0]}-${P.hr[1]} bpm).`]);
  if (R.irregular) o.push(["alert", "Irregular beat-to-beat rhythm was flagged; confirmation with an ECG is recommended."]);
  if (isNum(R.rmssd)) {
    if (P.rmssd > 0 && R.rmssd < P.rmssd) o.push(["warn", `Short-term HRV is low (RMSSD ${f(R.rmssd, 0)} ms, typical at rest above about ${P.rmssd} ms).`]);
    else o.push(["ok", `Short-term HRV: RMSSD ${f(R.rmssd, 0)} ms, SDNN ${f(R.sdnn, 0)} ms.`]);
  }
  if (!isNum(R.rr)) o.push(["alert", "Breathing rate could not be measured reliably."]);
  else if (R.rrState === "normal") o.push(["ok", `Breathing rate ${f(R.rr, 1)} /min, within the resting range for this age (${P.rr[0]}-${P.rr[1]} /min).`]);
  else o.push([R.rrState === "high" ? "alert" : "warn", `Breathing rate ${f(R.rr, 1)} /min is ${R.rrState === "high" ? "above" : "below"} the resting range for this age (${P.rr[0]}-${P.rr[1]} /min).`]);
  if (R.pauses.length) o.push(["warn", `${R.pauses.length} breathing pause${R.pauses.length > 1 ? "s" : ""} of ${P.apnea} s or longer (longest ${f(R.longestPause, 0)} s).`]);
  else if (isNum(R.rr)) o.push(["ok", `No breathing pauses of ${P.apnea} s or longer.`]);
  if (isNum(R.perCV) && R.perCV > 0.35) o.push(["warn", `Irregular breathing timing (cycle variation ${f(R.perCV * 100, 0)} %).`]);
  if (isNum(R.ie) && R.ie < 0.33) o.push(["warn", "Markedly prolonged expiration (I:E beyond 1:3), as can occur with airway narrowing."]);
  if (isNum(R.rsa)) o.push(["info", `Respiratory sinus arrhythmia ${f(R.rsa, 1)} bpm per breath; ${f(R.hrPerBreath, 1)} heart beats per breath.`]);
  if (R.motionSec > 30) o.push(["info", `Movement during ${f(R.motionSec, 0)} s of the recording was excluded from analysis.`]);
  o.push(["info", `Mean signal quality: heart ${f(R.sqiH, 0)}/100, breathing ${f(R.sqiR, 0)}/100.`]);
  return o;
}

/* ------------------------- minimal PDF writer ------------------------- */
const HELV = [278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,556,556,333,500,278,556,500,722,500,500,500,334,260,334,584];
const HELVB = [278,333,474,556,556,889,722,238,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,333,333,584,584,584,611,975,722,722,722,722,667,611,778,722,278,556,722,611,833,722,778,667,778,722,667,611,722,667,944,667,667,611,333,278,333,584,556,333,556,611,556,611,556,333,611,611,278,278,556,278,889,611,611,611,611,389,556,333,611,556,778,556,556,500,389,280,389,584];
function pdfSafe(s) {
  return String(s).replace(/[\u2013\u2014\u2212]/g, "-").replace(/[\u2018\u2019]/g, "'").replace(/[\u201C\u201D]/g, '"')
    .replace(/\u2265/g, ">=").replace(/\u2264/g, "<=").replace(/\u2026/g, "...").replace(/[^\x20-\x7E\xA0-\xFF]/g, "?");
}
class Pdf {
  constructor() { this.W = 595.28; this.H = 841.89; this.pages = []; this.c = null; }
  page() { this.c = []; this.pages.push(this.c); return this; }
  col(hex) { const n = parseInt(hex.slice(1), 16); return [(n >> 16 & 255) / 255, (n >> 8 & 255) / 255, (n & 255) / 255].map(x => x.toFixed(3)).join(" "); }
  n(x) { return (Math.round(x * 100) / 100).toString(); }
  width(s, size, bold) {
    const t = bold ? HELVB : HELV; let w = 0;
    for (const ch of pdfSafe(s)) { const c = ch.charCodeAt(0); w += (c >= 32 && c <= 126) ? t[c - 32] : c === 0xB0 ? 400 : 556; }
    return w * size / 1000;
  }
  text(x, y, s, o = {}) {
    const size = o.size || 9, bold = !!o.bold;
    s = pdfSafe(s);
    let xx = x;
    if (o.align === "right") xx = x - this.width(s, size, bold);
    else if (o.align === "center") xx = x - this.width(s, size, bold) / 2;
    const e = s.replace(/\\/g, "\\\\").replace(/\(/g, "\\(").replace(/\)/g, "\\)");
    this.c.push(`BT /${bold ? "F2" : "F1"} ${size} Tf ${this.col(o.color || "#1E2533")} rg 1 0 0 1 ${this.n(xx)} ${this.n(this.H - y)} Tm (${e}) Tj ET`);
  }
  wrap(s, maxW, size, bold) {
    const words = pdfSafe(s).split(/\s+/), lines = []; let cur = "";
    for (const w of words) {
      const t = cur ? cur + " " + w : w;
      if (this.width(t, size, bold) <= maxW || !cur) cur = t; else { lines.push(cur); cur = w; }
    }
    if (cur) lines.push(cur);
    return lines;
  }
  para(x, y, s, maxW, o = {}) {
    const size = o.size || 9, lh = o.lh || size * 1.35;
    for (const ln of this.wrap(s, maxW, size, o.bold)) { this.text(x, y, ln, o); y += lh; }
    return y;
  }
  line(x1, y1, x2, y2, w = 0.5, col = "#1E2533", dash) {
    this.c.push(`q ${this.col(col)} RG ${w} w ${dash ? "[" + dash + "] 0 d " : ""}${this.n(x1)} ${this.n(this.H - y1)} m ${this.n(x2)} ${this.n(this.H - y2)} l S Q`);
  }
  rect(x, y, w, h, o = {}) {
    let op = "";
    if (o.fill) op += `${this.col(o.fill)} rg `;
    if (o.stroke) op += `${this.col(o.stroke)} RG ${o.w || 0.5} w `;
    const paint = o.fill && o.stroke ? "B" : o.fill ? "f" : "S";
    this.c.push(`q ${op}${this.n(x)} ${this.n(this.H - y - h)} ${this.n(w)} ${this.n(h)} re ${paint} Q`);
  }
  poly(pts, col = "#1E2533", w = 0.6) {
    if (pts.length < 2) return;
    const s = [`q ${this.col(col)} RG ${w} w 1 J 1 j`];
    let pen = false;
    for (const p of pts) {
      if (!p) { pen = false; continue; }
      s.push(`${this.n(p[0])} ${this.n(this.H - p[1])} ${pen ? "l" : "m"}`);
      pen = true;
    }
    s.push("S Q");
    this.c.push(s.join("\n"));
  }
  dot(x, y, r, col) {
    const k = 0.5523 * r, X = x, Y = this.H - y, n = v => this.n(v);
    this.c.push(`q ${this.col(col)} rg ${n(X + r)} ${n(Y)} m ${n(X + r)} ${n(Y + k)} ${n(X + k)} ${n(Y + r)} ${n(X)} ${n(Y + r)} c ${n(X - k)} ${n(Y + r)} ${n(X - r)} ${n(Y + k)} ${n(X - r)} ${n(Y)} c ${n(X - r)} ${n(Y - k)} ${n(X - k)} ${n(Y - r)} ${n(X)} ${n(Y - r)} c ${n(X + k)} ${n(Y - r)} ${n(X + r)} ${n(Y - k)} ${n(X + r)} ${n(Y)} c f Q`);
  }
  build(title) {
    const objs = [];
    const add = s => { objs.push(s); return objs.length; };
    const catalog = add(""), pagesId = add("");
    const f1 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    const f2 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    const kids = [];
    for (const pg of this.pages) {
      const content = pg.join("\n");
      const cid = add(`<< /Length ${content.length} >>\nstream\n${content}\nendstream`);
      kids.push(add(`<< /Type /Page /Parent ${pagesId} 0 R /MediaBox [0 0 ${this.W} ${this.H}] /Resources << /Font << /F1 ${f1} 0 R /F2 ${f2} 0 R >> >> /Contents ${cid} 0 R >>`));
    }
    objs[catalog - 1] = `<< /Type /Catalog /Pages ${pagesId} 0 R >>`;
    objs[pagesId - 1] = `<< /Type /Pages /Kids [${kids.map(k => k + " 0 R").join(" ")}] /Count ${kids.length} >>`;
    const esc = s => pdfSafe(s).replace(/\\/g, "\\\\").replace(/\(/g, "\\(").replace(/\)/g, "\\)");
    const info = add(`<< /Title (${esc(title || "CardioResp report")}) /Producer (CardioResp Monitor) >>`);
    let out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    const offs = [];
    objs.forEach((o, i) => { offs.push(out.length); out += `${i + 1} 0 obj\n${o}\nendobj\n`; });
    const xref = out.length;
    out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
    for (const o of offs) out += String(o).padStart(10, "0") + " 00000 n \n";
    out += `trailer\n<< /Size ${objs.length + 1} /Root ${catalog} 0 R /Info ${info} 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
    const b = new Uint8Array(out.length);
    for (let i = 0; i < out.length; i++) b[i] = out.charCodeAt(i) & 255;
    return b;
  }
}

/* draw an ECG-style grid; returns mapping helpers */
function pdfGrid(pdf, x, y, w, h, xMajor, xMinor, x0v, x1v) {
  pdf.rect(x, y, w, h, {fill:"#FFF8F6"});
  const pxPer = w / (x1v - x0v);
  for (let v = Math.ceil(x0v / xMinor) * xMinor; v <= x1v + 1e-9; v += xMinor) {
    const X = x + (v - x0v) * pxPer;
    const major = Math.abs(v / xMajor - Math.round(v / xMajor)) < 1e-6;
    pdf.line(X, y, X, y + h, major ? 0.45 : 0.2, major ? "#E8A89C" : "#F5D2CB");
  }
  const rows = Math.max(2, Math.round(h / (w / ((x1v - x0v) / xMinor))));
  for (let i = 0; i <= rows; i++) {
    const Y = y + i * h / rows;
    pdf.line(x, Y, x + w, Y, (i % 5 === 0) ? 0.45 : 0.2, (i % 5 === 0) ? "#E8A89C" : "#F5D2CB");
  }
  pdf.rect(x, y, w, h, {stroke:"#D9A79E", w:0.6});
}

/* decimate to min/max pairs per horizontal bin for compact, faithful plots */
function minmaxPoints(vals, x, y, w, h, lo, hi, bins) {
  const n = vals.length; if (!n) return [];
  const pts = [], per = n / bins;
  const Y = v => y + h - (Math.max(lo, Math.min(hi, v)) - lo) / (hi - lo) * h;
  for (let b = 0; b < bins; b++) {
    const a = Math.floor(b * per), e = Math.max(a + 1, Math.floor((b + 1) * per));
    let mn = Infinity, mx = -Infinity, first = null;
    for (let i = a; i < e && i < n; i++) { const v = vals[i]; if (v === null || !isFinite(v)) continue; if (first === null) first = v; mn = Math.min(mn, v); mx = Math.max(mx, v); }
    if (first === null) { pts.push(null); continue; }
    const X = x + (b + 0.5) / bins * w;
    pts.push([X, Y(first > (mn + mx) / 2 ? mx : mn)]);
    pts.push([X, Y(first > (mn + mx) / 2 ? mn : mx)]);
  }
  return pts;
}

function buildPdf(L, R, pt, captureDate) {
  const pdf = new Pdf();
  const M = 40, W = pdf.W - 2 * M;
  const P = R.P;
  const wall = tMs => new Date(captureDate.getTime() - (L.now - tMs));
  const p2 = v => String(v).padStart(2, "0");            // fixed format: any phone language prints correctly in the PDF
  const tfmt = d => `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`;
  const dfmt = d => `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())}`;
  const footer = (i, n) => {
    pdf.line(M, 812, M + W, 812, 0.4, "#C9B7B2");
    pdf.text(M, 824, `CardioResp Monitor ${L.fw} - ${L.dev} - generated ${dfmt(captureDate)} ${tfmt(captureDate)}`, {size:7, color:"#5E6676"});
    pdf.text(M + W, 824, `Page ${i} of ${n} - not a certified medical device`, {size:7, color:"#5E6676", align:"right"});
  };
  const secTitle = (y, s) => { pdf.text(M, y, s, {size:11, bold:true}); pdf.line(M, y + 4, M + W, y + 4, 0.6, "#B42332"); return y + 18; };
  const NP = 3;

  /* ---------------- page 1: summary ---------------- */
  pdf.page();
  pdf.rect(0, 0, pdf.W, 70, {fill:"#FBF1EF"});
  pdf.text(M, 34, "Cardio-respiratory recording", {size:18, bold:true});
  pdf.text(M, 52, "Seismocardiography (chest vibration) and chest-wall breathing motion, 10-minute capture", {size:9, color:"#5E6676"});
  pdf.text(M + W, 34, L.dev, {size:9, bold:true, align:"right"});
  pdf.text(M + W, 48, `Chest motion sensor, ${f(L.fs, 1)} Hz`, {size:8, color:"#5E6676", align:"right"});
  let y = 92;
  const kv = (x, yy, k, v) => { pdf.text(x, yy, k, {size:8, color:"#5E6676"}); pdf.text(x + 78, yy, v || "--", {size:9}); };
  kv(M, y, "Name", pt.name); kv(M + W / 2, y, "Patient ID", pt.id); y += 14;
  kv(M, y, "Age", `${ageText(L.age)} - ${P.name}`); kv(M + W / 2, y, "Sex", pt.sex || "not stated"); y += 14;
  kv(M, y, "Sensor placement", pt.place); kv(M + W / 2, y, "Posture", pt.post); y += 14;
  kv(M, y, "Recorded", `${dfmt(wall(R.t0))} ${tfmt(wall(R.t0))} - ${tfmt(wall(R.t1))}`); kv(M + W / 2, y, "Duration", `${f(R.durMin, 1)} min`); y += 14;
  if (pt.notes) { pdf.text(M, y, "Notes", {size:8, color:"#5E6676"}); y = pdf.para(M + 78, y, pt.notes, W - 78, {size:9}); }
  y += 8;
  y = secTitle(y, "Summary");
  const rows = [
    ["Heart rate, mean", `${f(R.hrMean, 0)} bpm`, `${P.hr[0]}-${P.hr[1]} bpm`, R.hrState],
    ["Heart rate range (5th-95th pct.)", `${f(R.hrMin, 0)}-${f(R.hrMax, 0)} bpm`, "", null],
    ["RMSSD / SDNN / pNN50", `${f(R.rmssd, 0)} / ${f(R.sdnn, 0)} ms / ${f(R.pnn50, 0)} %`, P.rmssd ? `RMSSD >= ${P.rmssd} ms` : "age dependent", P.rmssd && isNum(R.rmssd) ? (R.rmssd < P.rmssd ? "low" : "normal") : null],
    ["Beats analysed (accepted)", `${R.nBeats} (${f(R.validPct, 0)} %)`, "", null],
    ["Breathing rate", `${f(R.rr, 1)} /min`, `${P.rr[0]}-${P.rr[1]} /min`, R.rrState],
    ["Breathing cycle variation", isNum(R.perCV) ? `${f(R.perCV * 100, 0)} %` : "--", "< 35 %", isNum(R.perCV) ? (R.perCV > 0.35 ? "high" : "normal") : null],
    ["Inspiration / expiration", `${f(R.ti, 1)} s / ${f(R.te, 1)} s (1:${isNum(R.ie) ? f(1 / R.ie, 1) : "--"})`, "about 1:1 - 1:3", isNum(R.ie) ? (R.ie < 0.33 ? "long expiration" : R.ie > 1.05 ? "long inspiration" : "normal") : null],
    ["Breathing depth (chest tilt)", `${f(R.amp, 2)} deg (variation ${isNum(R.ampCV) ? f(R.ampCV * 100, 0) : "--"} %)`, "relative", null],
    [`Pauses >= ${P.apnea} s`, `${R.pauses.length}${R.pauses.length ? ` (longest ${f(R.longestPause, 0)} s)` : ""}`, "none", R.pauses.length ? "present" : "normal"],
    ["Sighs / posture changes", `${R.sighs} / ${R.postureChanges}`, "", null],
    ["Resp. sinus arrhythmia", `${f(R.rsa, 1)} bpm per breath`, "", null],
    ["Heart beats per breath", f(R.hrPerBreath, 1), "about 4-5 (adult)", null],
    ["Movement excluded", `${f(R.motionSec, 0)} s`, "", null],
    ["Signal quality heart / breathing", `${f(R.sqiH, 0)} / ${f(R.sqiR, 0)} of 100`, ">= 60 good", null]
  ];
  pdf.text(M, y, "Measure", {size:8, bold:true, color:"#5E6676"});
  pdf.text(M + 190, y, "Value", {size:8, bold:true, color:"#5E6676"});
  pdf.text(M + 360, y, "Reference for age", {size:8, bold:true, color:"#5E6676"});
  pdf.text(M + W, y, "Assessment", {size:8, bold:true, color:"#5E6676", align:"right"});
  y += 5;
  for (const [k, v, ref, st] of rows) {
    pdf.line(M, y, M + W, y, 0.3, "#E7D8D3");
    y += 11;
    pdf.text(M, y, k, {size:9});
    pdf.text(M + 190, y, v, {size:9, bold:true});
    pdf.text(M + 360, y, ref, {size:8, color:"#5E6676"});
    if (st) pdf.text(M + W, y, st === "normal" ? "within range" : st === "high" ? "above range" : st === "low" ? "below range" : st, {size:8.5, bold:st !== "normal", color:st === "normal" ? "#2F7D4F" : "#B42332", align:"right"});
    y += 3;
  }
  pdf.line(M, y, M + W, y, 0.3, "#E7D8D3");
  y += 18;
  y = secTitle(y, "Automated interpretation");
  for (const [lvl, t] of R.findings) {
    const col = lvl === "ok" ? "#2F7D4F" : lvl === "alert" ? "#B42332" : lvl === "warn" ? "#9A5F0C" : "#5E6676";
    pdf.dot(M + 3, y - 3, 2.2, col);
    y = pdf.para(M + 12, y, t, W - 12, {size:9}) + 2;
  }
  // HR / RR trend chart
  y += 6;
  if (y < 640) {
    y = secTitle(y, "Heart and breathing rate, per second");
    const ch = Math.min(120, 790 - y - 20), cx = M + 26, cw = W - 52;
    const tr = (L.trend || []).filter(p => p[0] >= R.t0);
    pdf.rect(cx, y, cw, ch, {stroke:"#D9CFCB", w:0.5});
    const hrs = tr.map(p => p[1] > 0 && p[3] >= 15 ? p[1] / 10 : null).filter(v => v !== null);
    const rrs = tr.map(p => p[2] > 0 && p[4] >= 15 ? p[2] / 10 : null).filter(v => v !== null);
    const hLo = Math.floor(Math.min(P.hr[0], ...(hrs.length ? hrs : [P.hr[0]])) / 10) * 10 - 10;
    const hHi = Math.ceil(Math.max(P.hr[1], ...(hrs.length ? hrs : [P.hr[1]])) / 10) * 10 + 10;
    const rHi = Math.ceil(Math.max(P.rr[1] * 1.5, ...(rrs.length ? rrs : [0])) / 5) * 5;
    const X = t => cx + (t - R.t0) / Math.max(1, R.t1 - R.t0) * cw;
    // normal bands
    pdf.rect(cx, y + ch - (P.hr[1] - hLo) / (hHi - hLo) * ch, cw, (P.hr[1] - P.hr[0]) / (hHi - hLo) * ch, {fill:"#FBEDEF"});
    for (let k = 0; k <= 4; k++) {
      const yy = y + ch - k / 4 * ch;
      pdf.line(cx, yy, cx + cw, yy, 0.2, "#E7D8D3");
      pdf.text(cx - 3, yy + 3, f(hLo + k / 4 * (hHi - hLo), 0), {size:7, color:"#B42332", align:"right"});
      pdf.text(cx + cw + 3, yy + 3, f(k / 4 * rHi, 0), {size:7, color:"#1D5FA8"});
    }
    const segs = (idx, sq, lo, hi) => tr.map(p => p[idx] > 0 && p[sq] >= 15 ? [X(p[0]), y + ch - (p[idx] / 10 - lo) / (hi - lo) * ch] : null);
    pdf.poly(segs(1, 3, hLo, hHi), "#B42332", 0.8);
    pdf.poly(segs(2, 4, 0, rHi), "#1D5FA8", 0.8);
    for (const e of R.pauses) pdf.rect(X(e.t), y, Math.max(1, X(e.t + e.dur) - X(e.t)), ch, {fill:"#F3C9CD"});
    for (let m = 0; m <= Math.floor(R.durMin + 0.01); m++) pdf.text(X(R.t0 + m * 60000), y + ch + 9, `${m}`, {size:7, color:"#5E6676", align:"center"});
    pdf.text(cx + cw, y + ch + 18, "minutes", {size:7, color:"#5E6676", align:"right"});
    pdf.text(cx, y - 4, "bpm", {size:7, color:"#B42332"});
    pdf.text(cx + cw, y - 4, "breaths/min", {size:7, color:"#1D5FA8", align:"right"});
  }
  footer(1, NP);

  /* ---------------- page 2: waveforms ---------------- */
  pdf.page();
  y = 44;
  y = secTitle(y, "Breathing waveform - chest-wall tilt, last 10 minutes");
  const rs = L.resp || {d:[], fs:10, t0:L.now};
  const rv = rs.d.map(v => v / 1000);
  const motion = new Uint8Array(rv.length);
  for (const [s, n] of (rs.mr || [])) for (let i = s; i < s + n && i < motion.length; i++) motion[i] = 1;
  const clean = rv.filter((v, i) => !motion[i]);
  const absS = clean.map(Math.abs).sort((a, b) => a - b);
  const lim = Math.max(0.15, absS.length ? absS[Math.floor(0.98 * (absS.length - 1))] * 1.15 : 1);
  const rowSec = 150, rowsN = 4, rh = 78;
  const fsR = rs.fs || 10;
  for (let r = 0; r < rowsN; r++) {
    const a = r * rowSec * fsR, e = Math.min(rv.length, (r + 1) * rowSec * fsR);
    const yy = y + r * (rh + 20);
    pdfGrid(pdf, M, yy, W, rh, 5, 1, 0, rowSec);
    if (a < rv.length) {
      const seg = rv.slice(a, e).map((v, i) => motion[a + i] ? null : v);
      // motion shading
      for (let i = a; i < e;) {
        if (!motion[i]) { i++; continue; }
        const s = i; while (i < e && motion[i]) i++;
        pdf.rect(M + (s - a) / (rowSec * fsR) * W, yy, (i - s) / (rowSec * fsR) * W, rh, {fill:"#E3E3E6"});
      }
      const segW = (e - a) / (rowSec * fsR) * W;
      pdf.poly(minmaxPoints(seg, M, yy, segW, rh, -lim, lim, Math.max(2, Math.round(segW))), "#1D5FA8", 0.7);
      const tStart = rs.t0 + a / fsR * 1000;
      pdf.text(M, yy + rh + 9, tfmt(wall(tStart)), {size:7, color:"#5E6676"});
      pdf.text(M + W, yy + rh + 9, `+${rowSec} s, full height ${f(2 * lim, 2)} deg, grid 1 s / 5 s`, {size:7, color:"#5E6676", align:"right"});
      for (const ev of R.pauses) {
        const s = (ev.t - tStart) / 1000, d2 = ev.dur / 1000;
        if (s + d2 < 0 || s > rowSec) continue;
        const x1 = M + Math.max(0, s) / rowSec * W, x2 = M + Math.min(rowSec, s + d2) / rowSec * W;
        pdf.line(x1, yy + 4, x2, yy + 4, 2, "#B42332");
        pdf.text(x1, yy + 13, `pause ${f(d2, 0)} s`, {size:7, bold:true, color:"#B42332"});
      }
    }
  }
  y += rowsN * (rh + 20) + 4;
  pdf.text(M, y, "Upward = inspiration. Grey = movement (excluded). Red bars = breathing pauses.", {size:7.5, color:"#5E6676"});
  y += 16;
  y = secTitle(y, "Heart vibration - last 10 seconds (100 Hz)");
  const st = L.strip || {a:[], g:[], fs:100};
  const drawStrip = (vals, yy, col, unit, scale) => {
    const v = vals.map(x => x * scale);
    const s2 = v.map(Math.abs).sort((a, b) => a - b);
    const m2 = Math.max(1e-3, s2.length ? s2[Math.floor(0.995 * (s2.length - 1))] * 1.1 : 1);
    pdfGrid(pdf, M, yy, W, 56, 1, 0.2, 0, 10);
    pdf.poly(minmaxPoints(v, M, yy, W * v.length / 1000, 56, -m2, m2, Math.round(W * v.length / 1000)), col, 0.6);
    pdf.text(M + 4, yy + 10, `${unit}, full height ${f(2 * m2, m2 < 1 ? 2 : 1)}`, {size:7, color:col});
  };
  drawStrip(st.a || [], y, "#B42332", "SCG (linear, mg)", 0.01);
  drawStrip(st.g || [], y + 62, "#6B4FA0", "GCG (rotation, deg/s)", 0.001);
  pdf.text(M, y + 128, "Dominant-axis band-pass (5-30 Hz) seismocardiogram and gyrocardiogram. Grid 0.2 s / 1 s.", {size:7.5, color:"#5E6676"});
  y += 144;
  // tachogram + poincare
  const vb2 = (L.beats || []).filter(b => (b[2] & 1) && b[1] > 0 && b[0] >= R.t0);
  const tw = W * 0.62, pw = W - tw - 16, th2 = Math.min(150, 790 - y - 16);
  if (th2 > 60) {
    pdf.text(M, y, "Beat-to-beat intervals (ms)", {size:9, bold:true});
    pdf.text(M + tw + 16, y, "Poincare plot", {size:9, bold:true});
    const yy = y + 8;
    pdf.rect(M, yy, tw, th2, {stroke:"#D9CFCB"});
    const ibs = vb2.map(b => b[1]);
    if (ibs.length > 2) {
      const step = (Math.max(...ibs) - Math.min(...ibs)) > 300 ? 100 : 50;
      const lo = Math.floor(Math.min(...ibs) / step) * step - step, hi = Math.ceil(Math.max(...ibs) / step) * step + step;
      for (let v = lo; v <= hi; v += step) { const Y = yy + th2 - (v - lo) / (hi - lo) * th2; pdf.line(M, Y, M + tw, Y, 0.2, "#E7D8D3"); pdf.text(M - 3, Y + 3, String(v), {size:6.5, color:"#5E6676", align:"right"}); }
      const Xt = t => M + (t - R.t0) / Math.max(1, R.t1 - R.t0) * tw;
      for (const b of vb2) pdf.dot(Xt(b[0]), yy + th2 - (b[1] - lo) / (hi - lo) * th2, 0.8, "#B42332");
      const py = yy, ps = Math.min(pw, th2);
      pdf.rect(M + tw + 16, py, ps, ps, {stroke:"#D9CFCB"});
      pdf.line(M + tw + 16, py + ps, M + tw + 16 + ps, py, 0.3, "#C9B7B2", "2 2");
      for (let i = 1; i < vb2.length; i++) {
        const a2 = vb2[i - 1][1], b2 = vb2[i][1];
        pdf.dot(M + tw + 16 + (a2 - lo) / (hi - lo) * ps, py + ps - (b2 - lo) / (hi - lo) * ps, 0.8, "#6B4FA0");
      }
      pdf.text(M + tw + 16, py + ps + 10, `SD1 ${f(R.sd1, 0)} ms, SD2 ${f(R.sd2, 0)} ms, axes ${lo}-${hi} ms`, {size:7, color:"#5E6676"});
    } else pdf.text(M + 8, yy + 20, "Not enough accepted beats.", {size:8, color:"#5E6676"});
  }
  footer(2, NP);

  /* ---------------- page 3: spectra, events, method ---------------- */
  pdf.page();
  y = 44;
  y = secTitle(y, "Frequency analysis");
  const sp = L.spec || {c:[], r:[], cd:0, rd:0};
  const spec = (x, yy, w, h, arr, df, mult, xmax, col, label, ticks, peak) => {
    pdf.rect(x, yy, w, h, {stroke:"#D9CFCB"});
    const n = arr.length;
    const pts = [];
    for (let i = 0; i < n; i++) { const fx = i * df * mult; if (fx > xmax) break; pts.push([x + fx / xmax * w, yy + h - arr[i] / 1000 * h]); }
    pdf.poly(pts, col, 0.8);
    for (const t of ticks) { const X = x + t / xmax * w; pdf.line(X, yy + h, X, yy + h + 3, 0.4, "#5E6676"); pdf.text(X, yy + h + 11, String(t), {size:7, color:"#5E6676", align:"center"}); }
    pdf.text(x, yy - 5, label, {size:8.5, bold:true});
    if (isNum(peak) && peak > 0) { const X = x + peak / xmax * w; pdf.line(X, yy, X, yy + h, 0.5, col, "2 2"); pdf.text(Math.min(X + 3, x + w - 40), yy + 10, `${f(peak, 1)}`, {size:7.5, bold:true, color:col}); }
  };
  const sw = (W - 20) / 2;
  spec(M, y + 10, sw, 110, sp.c || [], sp.cd, 60, 240, "#B42332", "Heart-beat rhythm (beats/min)", [0, 60, 120, 180, 240], L.res && L.res.hrs);
  spec(M + sw + 20, y + 10, sw, 110, sp.r || [], sp.rd, 60, 60, "#1D5FA8", "Breathing (breaths/min)", [0, 15, 30, 45, 60], L.res && L.res.rrs);
  y += 150;
  y = secTitle(y, "Events in this recording");
  const evs = R.events.filter(e => e.type !== 4 || e.t > R.t0);
  if (!evs.length) { pdf.text(M, y, "No pauses, movement or posture changes detected.", {size:9}); y += 14; }
  else {
    pdf.text(M, y, "Time", {size:8, bold:true, color:"#5E6676"}); pdf.text(M + 90, y, "Event", {size:8, bold:true, color:"#5E6676"}); pdf.text(M + 260, y, "Duration", {size:8, bold:true, color:"#5E6676"});
    y += 12;
    for (const e of evs.slice(0, 26)) {
      pdf.text(M, y, tfmt(wall(e.t)), {size:9}); pdf.text(M + 90, y, EVN[e.type] || "Event", {size:9, bold:e.type === 1, color:e.type === 1 ? "#B42332" : "#1E2533"});
      pdf.text(M + 260, y, e.dur ? `${f(e.dur / 1000, 1)} s` : "", {size:9});
      y += 12;
    }
    if (evs.length > 26) { pdf.text(M, y, `... ${evs.length - 26} more in the CSV file`, {size:8, color:"#5E6676"}); y += 12; }
  }
  y += 10;
  y = secTitle(y, "Method");
  const meth = [
    `A chest-worn 6-axis motion sensor (accelerometer and gyroscope) sampled at ${f(L.fs, 1)} Hz (range +-2 g and +-250 deg/s, 44 Hz anti-alias filter) was fixed to the chest wall. At start-up the gravity direction and gyroscope offsets were measured during ${8} s of stillness; the gravity vector is then tracked by fusing gyroscope rotation with the accelerometer.`,
    "Breathing is the small rotation of the chest wall relative to gravity. It is band-pass filtered for the patient's age, projected on its principal 3-D axis and segmented breath by breath (inspiration, expiration, depth). The breathing rate is the number of breaths per minute of movement-free time, so pauses lower it.",
    "Heart beats are detected from chest vibration (seismocardiogram, 5-30 Hz) on the accelerometer and from chest rotation (gyrocardiogram) on the gyroscope. Each sensor's dominant axis is found adaptively, the two energy envelopes are fused by periodicity-based quality weights and beats are detected with adaptive thresholds, refractory period, search-back and interval validation. Heart rate is cross-checked by autocorrelation and spectral analysis.",
    "Movement is detected from gravity-free linear acceleration, fast chest rotation and large turns, so breathing itself (also deep or fast breathing) is not taken for movement; affected segments are excluded. When strong breathing shakes the chest, the heart rate is paused instead of locking onto the breathing rate. Coughs are counted only as short explosive vibration bursts with quiet before and after (body movements and taps are rejected), and illness patterns are only suggested after 5-30 minutes of calm monitoring. Reference ranges are awake resting values for the entered age (PALS/AHA); interpretation text is automated and must be reviewed by a clinician."
  ];
  for (const m of meth) y = pdf.para(M, y, m, W, {size:8.5, color:"#1E2533"}) + 4;
  y += 16;
  pdf.line(M, y + 24, M + 220, y + 24, 0.5, "#1E2533");
  pdf.line(M + 280, y + 24, M + W, y + 24, 0.5, "#1E2533");
  pdf.text(M, y + 34, "Reviewed by (clinician)", {size:8, color:"#5E6676"});
  pdf.text(M + 280, y + 34, "Date / comments", {size:8, color:"#5E6676"});
  y += 56;
  pdf.para(M, Math.min(y, 770), "CardioResp is a research and wellness instrument and not a certified medical device. It does not replace ECG, pulse oximetry, polysomnography or clinical examination. Readings depend on sensor contact, placement and stillness.", W, {size:7.5, color:"#5E6676"});
  footer(3, NP);
  return pdf.build(`CardioResp report ${pt.name || ""} ${dfmt(captureDate)}`);
}

/* ------------------------------- CSV ------------------------------- */
function buildCsv(L, R, pt, captureDate) {
  const q = s => { s = String(s == null ? "" : s); return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s; };
  const rel = t => ((t - R.t0) / 1000).toFixed(2);
  const o = [];
  o.push("# CardioResp Monitor export");
  o.push(`# device,${q(L.dev)},firmware,${q(L.fw)},sensor,${q(L.model || "motion sensor")},sample_rate_hz,${f(L.fs, 3)}`);
  o.push(`# captured,${captureDate.toISOString()},start_offset_s,0,duration_min,${f(R.durMin, 2)}`);
  o.push(`# name,${q(pt.name)},id,${q(pt.id)},age_years,${f(L.age, 2)},age_group,${q(R.P.name)},sex,${q(pt.sex)},placement,${q(pt.place)},posture,${q(pt.post)}`);
  if (pt.notes) o.push(`# notes,${q(pt.notes)}`);
  o.push("", "[summary]", "measure,value,unit");
  const S = [["heart_rate_mean", f(R.hrMean, 1), "bpm"], ["heart_rate_p5", f(R.hrMin, 1), "bpm"], ["heart_rate_p95", f(R.hrMax, 1), "bpm"],
    ["rmssd", f(R.rmssd, 1), "ms"], ["sdnn", f(R.sdnn, 1), "ms"], ["pnn50", f(R.pnn50, 1), "%"], ["sd1", f(R.sd1, 1), "ms"], ["sd2", f(R.sd2, 1), "ms"],
    ["beats_detected", R.nBeats, ""], ["beats_accepted_pct", f(R.validPct, 1), "%"], ["breathing_rate", f(R.rr, 2), "/min"],
    ["breaths_valid", R.nBreaths, ""], ["breath_cycle_cv", f(R.perCV * 100, 1), "%"], ["inspiration_mean", f(R.ti, 2), "s"], ["expiration_mean", f(R.te, 2), "s"],
    ["ie_ratio", f(R.ie, 3), ""], ["depth_mean", f(R.amp, 3), "deg"], ["depth_cv", f(R.ampCV * 100, 1), "%"], ["pauses", R.pauses.length, ""],
    ["longest_pause", f(R.longestPause, 1), "s"], ["sighs", R.sighs, ""], ["rsa", f(R.rsa, 2), "bpm"], ["beats_per_breath", f(R.hrPerBreath, 2), ""],
    ["movement_excluded", f(R.motionSec, 1), "s"], ["sqi_heart_mean", f(R.sqiH, 1), "/100"], ["sqi_breathing_mean", f(R.sqiR, 1), "/100"]];
  for (const r of S) o.push(r.join(","));
  o.push("", "[trend_1hz]", "t_s,hr_bpm,rr_per_min,sqi_heart,sqi_breathing,motion,pause,posture_change,tilt_deg");
  for (const p of L.trend || []) if (p[0] >= R.t0) o.push([rel(p[0]), p[1] ? (p[1] / 10).toFixed(1) : "", p[2] ? (p[2] / 10).toFixed(1) : "", p[3], p[4], p[5] & 1 ? 1 : 0, p[5] & 2 ? 1 : 0, p[5] & 4 ? 1 : 0, p[6]].join(","));
  o.push("", "[beats]", "t_s,ibi_ms,accepted,search_back,envelope_amp");
  for (const b of L.beats || []) if (b[0] >= R.t0) o.push([rel(b[0]), b[1] || "", b[2] & 1 ? 1 : 0, b[2] & 2 ? 1 : 0, b[3]].join(","));
  o.push("", "[breaths]", "t_end_s,cycle_s,inspiration_s,expiration_s,depth_deg,valid,motion,sigh");
  for (const b of L.breaths || []) if (b[0] >= R.t0) o.push([rel(b[0]), (b[1] / 1000).toFixed(2), (b[2] / 1000).toFixed(2), (b[3] / 1000).toFixed(2), (b[4] / 1000).toFixed(3), b[5] & 1 ? 1 : 0, b[5] & 2 ? 1 : 0, b[5] & 4 ? 1 : 0].join(","));
  o.push("", "[events]", "t_s,event,duration_s");
  for (const e of R.events) o.push([rel(e.t), q(EVN[e.type] || e.type), (e.dur / 1000).toFixed(1)].join(","));
  const rs = L.resp || {d:[], fs:10, t0:0};
  const mo = new Uint8Array(rs.d.length);
  for (const [s, n] of rs.mr || []) for (let i = s; i < s + n && i < mo.length; i++) mo[i] = 1;
  o.push("", "[breathing_waveform_10hz]", "t_s,tilt_mdeg,motion");
  rs.d.forEach((v, i) => o.push(`${rel(rs.t0 + i * 1000 / rs.fs)},${v},${mo[i]}`));
  const st = L.strip || {a:[], g:[], fs:100, t0:0};
  o.push("", "[heart_vibration_100hz]", "t_s,scg_mg,gcg_dps");
  (st.a || []).forEach((v, i) => o.push(`${rel(st.t0 + i * 10)},${(v / 100).toFixed(2)},${((st.g[i] || 0) / 1000).toFixed(3)}`));
  return o.join("\r\n") + "\r\n";
}

if (typeof module !== "undefined") module.exports = {AGE_TABLE, ageProfile, liveAnalysis, computeReport, buildPdf, buildCsv, computeRSA, Pdf, PF, f, isNum};
