"use strict";
/* alarms.js - CardioResp v1.3: typed sound alarms.
   Every kind of alarm has its own sound, so a nurse can tell from the sound alone what is wrong:
     urgent  (priority 3) : heart signal lost, long breathing pause, heart/breathing rate far outside the range -> every 4 s
     warning (priority 2) : heart rate or rhythm, breathing rate or pattern, short breathing pause,
                            sensor lost, no data from the monitor, no signal from the body (v1.3)               -> every 10 s
     notice  (priority 1) : temperature, chest vibration / cough, measurement stopped elsewhere,
                            battery low (v1.3)                                                                  -> every 20 s
   Only the most urgent alarm that is allowed to sound is played; all active alarms are listed on the alarm bar.
   A condition must last for a short hold time before its alarm starts (no alarm from one odd reading), and a single
   missing reading does not reset or clear an alarm (3 s grace).
   Silence mutes the current alarms for 2 minutes; a NEW alarm, an alarm that comes back, or one that gets worse still sounds.
   Browsers allow sound only after the page has been tapped once; the alarm bar says so until sound is ready.
   Not a certified medical alarm system. */
(function () {
const $ = id => document.getElementById(id);
const num = v => typeof v === "number" && isFinite(v);
const TYPES = [
  {k: "cardiac", short: "Heart signal", name: "Heart signal lost", lock: true, lic: true, prio: 3,
   when: "the assessment reports that the heartbeat vibration was lost while the person lies still (possible cardiac arrest)",
   snd: {w: "square", n: [[988, .11, .07], [988, .11, .07], [988, .11, .26], [988, .11, .07], [988, .11, 0]]}, desc: "fast high burst (3 + 2)"},
  {k: "apnea", short: "Pause", name: "Breathing pause", lock: true, prio: 2,
   when: "no breath for longer than the pause limit of the age group (warning); urgent after 20 s",
   snd: {w: "triangle", n: [[880, .2, .04], [587, .2, .04], [880, .2, .04], [587, .2, 0]]}, desc: "high-low siren"},
  {k: "heart", short: "Heart", name: "Heart rate or rhythm", prio: 2,
   when: "heart rate clearly outside the age range for 10 s (urgent when far outside), or an irregular-rhythm warning",
   snd: {w: "sine", n: [[784, .15, .11], [784, .15, .11], [784, .15, 0]]}, desc: "three equal beeps"},
  {k: "breath", short: "Breathing", name: "Breathing rate or pattern", prio: 2,
   when: "last-minute breathing rate clearly outside the age range for 10 s (urgent when far outside), laboured breathing or repeated pauses",
   snd: {w: "sine", n: [[659, .24, .05], [523, .24, .05], [440, .34, 0]]}, desc: "three falling tones"},
  {k: "temp", short: "Temperature", name: "Body temperature", prio: 1,
   when: "38.0 °C (100.4 °F) or more, or below 34.5 °C (94.1 °F), for 20 s (warning from 39.5 °C / 103.1 °F) - only after the temperature has been calibrated in Settings",
   snd: {w: "triangle", n: [[523, .42, .14], [392, .56, 0]]}, desc: "two slow tones"},
  {k: "vib", short: "Cough / vibration", name: "Chest vibration or coughing", lic: true, prio: 1,
   when: "repeated cough bursts, or strong chest vibration while the person is still, for 5 s",
   snd: {w: "sine", n: [[450, .22, .08, 900], [450, .22, 0, 900]]}, desc: "two rising chirps"},
  {k: "sensor", short: "Sensor", name: "Sensor lost", lock: true, prio: 2,
   when: "the sensor cable is disconnected or the sensor does not answer for 3 s",
   snd: {w: "square", n: [[330, .45, .22], [330, .45, 0]]}, desc: "two low buzzes"},
  {k: "conn", short: "Connection", name: "No data from the monitor", lock: true, prio: 2,
   when: "nothing received from the monitor for 15 s (urgent if an urgent alarm was active)",
   snd: {w: "square", n: [[294, .38, .16], [294, .38, .16], [294, .38, 0]]}, desc: "three low buzzes"},
  {k: "nosig", short: "No body signal", name: "No signal from the body", prio: 2,
   when: "while measuring, neither heartbeat nor breathing is found for 30 s although the person is still (sensor loose or off the chest)",
   snd: {w: "triangle", n: [[392, .3, .1], [494, .3, .1], [392, .3, 0]]}, desc: "low-high-low"},
  {k: "stopped", short: "Stopped", name: "Measurement stopped", prio: 1,
   when: "the measurement was stopped at the monitor (button) or from another phone, so the person is no longer watched",
   snd: {w: "sine", n: [[587, .3, .12], [440, .45, 0]]}, desc: "two falling tones"},
  {k: "batt", short: "Battery", name: "Battery low", prio: 1,
   when: "the monitor's battery is at 15 % or less for 30 s (warning at 5 %)",
   snd: {w: "sine", n: [[523, .12, .08], [523, .12, 0]]}, desc: "two short pips"}
];
const BYK = {}; TYPES.forEach((t, i) => { BYK[t.k] = t; t.i = i; });
const REP = {3: 4000, 2: 10000, 1: 20000};
const PN = {3: "urgent", 2: "warning", 1: "notice"};
const GRACE = 3000;
const A = {on: true, vol: 0.7, vib: true, en: {}, silUntil: 0, sil: new Map(), act: new Map(), pend: new Map(), miss: new Map(), last: {},
           hideUntil: 0, statusAt: 0, everStatus: false, lastS: null, ctx: null, full: true, lastVib: 0, dt: 1000, startAt: Date.now(), log: [],
           wasMeasuring: false, userStopAt: 0};
TYPES.forEach(t => { A.en[t.k] = true; });
try {   // the master switch is NOT remembered: alarms are always on after the page is opened again
  const s = JSON.parse(localStorage.getItem("cr_alarm") || "{}");
  if (num(s.vol)) A.vol = Math.max(0.1, Math.min(1, s.vol));
  if (typeof s.vib === "boolean") A.vib = s.vib;
  if (s.en) for (const k in s.en) if (k in A.en && !BYK[k].lock) A.en[k] = !!s.en[k];
} catch (e) {}
function save() { try { localStorage.setItem("cr_alarm", JSON.stringify({vol: A.vol, vib: A.vib, en: A.en})); } catch (e) {} }
const enabled = k => BYK[k].lock || A.en[k];

/* ------------------------------ sound ------------------------------ */
function ctx() {
  try {
    if (!A.ctx) {
      const C = window.AudioContext || window.webkitAudioContext; if (!C) return null;
      try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch (e) {}   // iOS: play even with the silent switch on
      A.ctx = new C();
    }
    if (A.ctx.state !== "running" && A.ctx.resume) { const p = A.ctx.resume(); if (p && p.catch) p.catch(() => {}); }
    return A.ctx;
  } catch (e) { return null; }
}
const ready = () => !!(A.ctx && A.ctx.state === "running");
["pointerdown", "keydown", "touchstart", "click"].forEach(ev => window.addEventListener(ev, () => { ctx(); setTimeout(() => render(Date.now()), 60); }, {passive: true}));
function play(k, gesture) {
  const t = BYK[k], c = gesture ? ctx() : A.ctx; if (!t || !c) return false;
  if (c.state !== "running") return false;               // never queue notes on a suspended clock (they would all play at once later)
  try {
    const master = c.createGain(); master.gain.value = 0.32 * A.vol; master.connect(c.destination);
    let at = c.currentTime + 0.02;
    for (const [f, d, gap, f2] of t.snd.n) {
      const o = c.createOscillator(), g = c.createGain(), pk = t.snd.w === "square" ? 0.45 : 1;
      o.type = t.snd.w; o.frequency.setValueAtTime(f, at);
      if (f2) o.frequency.linearRampToValueAtTime(f2, at + d);
      g.gain.setValueAtTime(0, at); g.gain.linearRampToValueAtTime(pk, at + 0.012);
      g.gain.setValueAtTime(pk, at + d - 0.025); g.gain.linearRampToValueAtTime(0, at + d);
      o.connect(g); g.connect(master); o.start(at); o.stop(at + d + 0.01);
      at += d + gap;
    }
    return true;
  } catch (e) { return false; }
}

/* ------------------------------ conditions ------------------------------ */
/* S = status message, X = {P (age profile), hrZ, zone(v, range), tz, tb, vz (vibration zones), v (verdict or null), full} */
function conditions(S, X) {
  const c = {}, v = X.v, code = v && v.code, vs = v ? (v.sev | 0) : 0, P = X.P;
  const zone = X.zone || (() => 0);
  const now = Date.now();
  if (S.cal === 3) c.sensor = {p: 2, t: /lost/i.test(S.calmsg || "") ? "sensor connection lost - check the cable" : "sensor not found - check the cable", hold: 3000};
  // v1.3: battery (only when a battery is fitted)
  if (S.bat && S.bat.ok && num(S.bat.p) && S.bat.p <= 15) c.batt = {p: S.bat.p <= 5 ? 2 : 1, t: `${S.bat.p} % left - connect the charger`, hold: 30000};
  // v1.3: the measurement stopped without this page asking for it (button on the monitor, another phone)
  if (S.cal === 2) A.wasMeasuring = true;
  else if (S.cal === 4 && A.wasMeasuring) {
    if (now - A.userStopAt <= 20000) A.wasMeasuring = false;  // stopped from this page on purpose: nothing to report
    else c.stopped = {p: 1, t: "the person is no longer watched - press Start measurement to continue", hold: 3000};
  }
  if (S.cal === 1 || S.cal === 0) A.wasMeasuring = false;  // a new start: the stop has been dealt with
  if (S.cal !== 2) return c;
  if (X.full && vs >= 3 && (code === "arrest" || code === "brady3")) c.cardiac = {p: 3, t: v.head || "heartbeat not detected", hold: 0};
  // v1.3: set points and time windows from the doctor / nurse settings (age-based defaults unless changed)
  const L = X.lim || defLimits(P);
  if (num(S.apc) && S.apc >= L.apnea) {
    const urgent = S.apc >= L.apneaU;
    c.apnea = {p: urgent ? 3 : 2, t: `no breath for ${Math.round(S.apc)} s (alarm after ${L.apnea} s)`, hold: 0};
  }
  // v1.3: no heart-rate alarm while strong breathing shakes the chest (the heart rate is paused then, not measured)
  const hrOk = S.pres !== 0 && num(S.hr) && (S.sqh | 0) >= 40 && !S.art;
  if (hrOk && (S.hr > L.hrHi || S.hr < L.hrLo)) {
    const hi = S.hr > L.hrHi, urg = S.hr > L.hrHiU || S.hr < L.hrLoU;
    c.heart = {p: urg ? 3 : 2, t: `${Math.round(S.hr)} bpm (alarm ${hi ? "above " + L.hrHi : "below " + L.hrLo}${urg ? ", urgent " + (hi ? "above " + L.hrHiU : "below " + L.hrLoU) : ""})`, hold: L.hrDly * 1000};
  } else if (X.full && vs >= 2 && code === "irreg" && hrOk) c.heart = {p: 2, t: v.head, hold: Math.max(10000, L.hrDly * 1000)};
  // breathing: the breaths counted in the last minute (a nurse's count); the newest breaths only before that exists
  const rrv = num(S.rr1) ? S.rr1 : num(S.rrn) ? S.rrn : S.rr;
  const rrOk = num(rrv) && (S.sqr | 0) >= 40;
  if (rrOk && (rrv > L.rrHi || rrv < L.rrLo)) {
    const hi = rrv > L.rrHi, urg = rrv > L.rrHiU || rrv < L.rrLoU;
    c.breath = {p: urg ? 3 : 2, t: `${num(S.rr1) ? "last minute " : ""}${rrv.toFixed(rrv < 10 ? 1 : 0)} /min (alarm ${hi ? "above " + L.rrHi : "below " + L.rrLo}${urg ? ", urgent " + (hi ? "above " + L.rrHiU : "below " + L.rrLoU) : ""})`, hold: L.rrDly * 1000};
  } else if (X.full && vs >= 2 && (code === "pauses" || code === "resp-inf" || code === "osa" || code === "csr" || (code === "labour" && rrOk))) c.breath = {p: 2, t: v.head, hold: Math.max(10000, L.rrDly * 1000)};
  // v1.3: nothing from the body although the person is still: the sensor is loose or off the chest
  const anyHeart = S.pres && num(S.hr), anyBreath = num(S.rr1) || num(S.rrn) || num(S.rr);
  if ((S.win | 0) >= 45 && !S.mot && !anyHeart && !anyBreath) c.nosig = {p: 2, t: "no heartbeat or breathing found - check that the sensor sits firmly on the chest", hold: L.nosigDly * 1000};
  if (X.full && X.vz) {
    const vz = X.vz;
    const strong = !S.mot && Math.max(vz.a, vz.pk) >= 2;   // vibration while the person moves is movement, not a finding
    if (vz.cg >= 2 || strong) c.vib = {p: 1, t: vz.cg >= 2 ? `${S.cgh | 0} cough bursts in 30 s` : `strong chest vibration (${num(S.vra) ? S.vra.toFixed(0) : "--"} mg)`, hold: 5000};
  }
  const tcal = S.tcal !== undefined ? !!S.tcal : num(S.tb) && num(S.temp) && Math.abs(S.tb - S.temp) > 0.05;   // calibrated against a thermometer
  if (tcal && num(X.tb) && (X.tb >= L.tHi || X.tb < L.tLo)) {           // "38.0 °C or more"
    const p = X.tb >= L.tHiU || X.tb < L.tLo ? 2 : 1;
    const lim = X.tb < L.tLo ? L.tLo : p === 2 ? L.tHiU : L.tHi;
    c.temp = {p, t: `${X.tb.toFixed(1)} °C / ${(X.tb * 9 / 5 + 32).toFixed(1)} °F (alarm ${X.tb < L.tLo ? "below" : "from"} ${lim.toFixed(1)} °C / ${(lim * 9 / 5 + 32).toFixed(1)} °F)`, hold: L.tDly * 1000};
  }
  return c;
}
/* v1.3 default limits = the age-based defaults of AlarmSettings.h (used when the page has not received the monitor's limits) */
function defLimits(P) {
  const r = Math.round;
  return {hrLo: r(0.83 * P.hr[0]), hrHi: r(1.2 * P.hr[1]), hrLoU: r(0.667 * P.hr[0]), hrHiU: r(1.5 * P.hr[1]), rrLo: r(0.83 * P.rr[0]), rrHi: r(1.2 * P.rr[1]),
    rrLoU: r(0.667 * P.rr[0]), rrHiU: r(1.5 * P.rr[1]), apnea: P.apnea, apneaU: Math.max(20, P.apnea), tLo: 34.5, tHi: 38.0, tHiU: 39.5, hrDly: 10, rrDly: 10, tDly: 20, nosigDly: 30};
}
/* the alarm table in Settings says when each alarm starts, with the limits in use */
let limSig = "";
function setLimits(L) {
  const sig = JSON.stringify(L); if (sig === limSig) return; limSig = sig;
  BYK.heart.when = `heart rate below ${L.hrLo} or above ${L.hrHi} bpm for ${L.hrDly} s (urgent below ${L.hrLoU} or above ${L.hrHiU}), or a lasting irregular rhythm`;
  BYK.breath.when = `breaths counted in the last minute below ${L.rrLo} or above ${L.rrHi} /min for ${L.rrDly} s (urgent below ${L.rrLoU} or above ${L.rrHiU}), laboured breathing or repeated pauses`;
  BYK.apnea.when = `no breath for ${L.apnea} s (urgent after ${L.apneaU} s)`;
  const cf = c => `${c.toFixed(1)} °C (${(c * 9 / 5 + 32).toFixed(1)} °F)`;
  BYK.temp.when = `${cf(L.tHi)} or more (warning from ${cf(L.tHiU)}), or below ${cf(L.tLo)}, for ${L.tDly} s - only after the temperature has been calibrated in Settings`;
  BYK.nosig.when = `while measuring, neither heartbeat nor breathing is found for ${L.nosigDly} s although the person is still (sensor loose or off the chest)`;
  buildTable(); syncControls();
}
/* event log: start, worst level and end of every alarm (shown under Detailed analysis, downloadable as CSV) */
function logStart(k, a, now) { A.log.push({k, name: BYK[k].name, p: a.p, t: a.t, t0: now, t1: 0}); if (A.log.length > 300) A.log.shift(); }
function logOpen(k) { for (let i = A.log.length - 1; i >= 0; i--) if (A.log[i].k === k && !A.log[i].t1) return A.log[i]; return null; }
function logWorse(k, a) { const e = logOpen(k); if (e && a.p > e.p) { e.p = a.p; e.t = a.t; } }
function clear(k, keepSilence) {
  if (A.act.has(k)) { const e = logOpen(k); if (e) e.t1 = Date.now(); }
  A.act.delete(k); A.pend.delete(k); A.miss.delete(k); if (!keepSilence) A.sil.delete(k);
}
/* Pending (not yet sounding): the condition must be present for most of the hold time - one missed reading is tolerated,
   two in a row restart the hold, and at least 70 % of the readings must show it.
   Active: it ends only when the condition has been absent for 3 s (or 2.5 status intervals in the slow power modes). */
function apply(c, now) {
  const gapPend = Math.max(1500, 1.6 * A.dt), gapAct = Math.max(GRACE, 2.5 * A.dt);
  for (const t of TYPES) {
    if (t.k === "conn") continue;
    const x = c[t.k];
    if (!x) {
      if (A.act.has(t.k)) {
        if (!A.miss.has(t.k)) A.miss.set(t.k, now);
        if (now - A.miss.get(t.k) >= gapAct) clear(t.k);    // gone: the alarm ends (a later episode counts as new)
      } else if (A.pend.has(t.k)) {
        const pd = A.pend.get(t.k); pd.m++;
        if (now - pd.last > gapPend) A.pend.delete(t.k);
      }
      continue;
    }
    A.miss.delete(t.k);
    let pd = A.pend.get(t.k);
    if (!pd) { pd = {t0: now, last: now, n: 0, m: 0}; A.pend.set(t.k, pd); }
    pd.n++; pd.last = now;
    const was = A.act.get(t.k);
    if (was || (now - pd.t0 >= x.hold && pd.n >= 0.7 * (pd.n + pd.m))) {
      A.act.set(t.k, {p: x.p, t: x.t, since: was ? was.since : now});
      if (!was) logStart(t.k, x, now); else if (was.p < x.p) logWorse(t.k, x);
      if (was && was.p < x.p) A.last[t.k] = 0;               // got worse: sound at once
    }
  }
}
function update(S, X) {
  if (!S || S === A.lastS) return;                          // the same message again (e.g. a repaint) is not new data
  const now = Date.now();
  if (A.everStatus) A.dt = 0.8 * A.dt + 0.2 * Math.min(8000, Math.max(200, now - A.statusAt));   // status interval (slower in Eco / when hot)
  A.lastS = S; A.statusAt = now; A.everStatus = true; A.full = !!X.full;
  if (A.act.has("conn")) clear("conn");
  let c = {};
  try { c = conditions(S, X); } catch (e) { c = {}; }
  apply(c, A.statusAt);
  tick();
}

/* ------------------------------ scheduler and UI ------------------------------ */
const silenced = now => now < A.silUntil;
function mayBreak(k, p) { return !A.sil.has(k) || p > A.sil.get(k); }     // new, returning or worse alarm
function pick(now) {
  let best = null;
  for (const [k, a] of A.act) {
    if (!enabled(k)) continue;
    if (silenced(now) && !mayBreak(k, a.p)) continue;
    if (!best || a.p > best.a.p || (a.p === best.a.p && BYK[k].i < BYK[best.k].i)) best = {k, a};
  }
  return best;
}
function tick() {
  const now = Date.now();
  const noData = A.everStatus ? now - A.statusAt > 15000 : now - A.startAt > 20000;
  if (noData) {                                            // no data: the other alarms are stale; keep their urgency on the connection alarm
    let pmax = 2;
    for (const [k, a] of A.act) if (k !== "conn" && a.p === 3) pmax = 3;
    const was = A.act.get("conn");
    for (const k of Array.from(A.act.keys())) if (k !== "conn") clear(k, true);
    const p = Math.max(pmax, was ? was.p : 2);
    A.act.set("conn", {p, t: A.everStatus ? `no data from the monitor for ${Math.round((now - A.statusAt) / 1000)} s` : "no data received since this page was opened", since: was ? was.since : now});
    if (!was) logStart("conn", {p, t: "no data from the monitor"}, now); else if (was.p < p) logWorse("conn", {p, t: "no data from the monitor (an urgent alarm was active)"});
    if (was && was.p < p) A.last.conn = 0;
  }
  const b = pick(now);
  if (b && A.on && now - (A.last[b.k] || 0) >= REP[b.a.p]) {
    if (play(b.k)) A.last[b.k] = now;
    if (b.a.p === 3 && A.vib && navigator.vibrate && now - A.lastVib >= REP[3]) { A.lastVib = now; try { navigator.vibrate([300, 120, 300, 120, 300]); } catch (e) {} }
  }
  render(now);
}
function render(now) {
  const bar = $("alarmBar"); if (!bar) return;
  const list = TYPES.filter(t => A.act.has(t.k)).map(t => ({t, a: A.act.get(t.k)})).sort((x, y) => y.a.p - x.a.p || x.t.i - y.t.i);
  const pmax = list.length ? list[0].a.p : 0;
  const sil = silenced(now), breaking = sil && list.some(x => enabled(x.t.k) && mayBreak(x.t.k, x.a.p));
  const closed = now < A.hideUntil && sil && !breaking;     // closed with x: hidden until a new / worse alarm or 2 min
  bar.hidden = list.length === 0 || closed;
  document.body.classList.toggle("alarmon", list.length > 0 && !closed);
  bar.className = "alarmbar p" + pmax + (sil && !breaking ? " sil" : "");
  const esc = s => String(s).replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));
  const muted = x => enabled(x.t.k) && A.on ? "" : " (sound off)";
  let html = list.length ? `<span class="a1"><b>${esc(list[0].t.name)}</b> - ${esc(list[0].a.t)}${muted(list[0])}</span>` +
    (list.length > 1 ? `<span class="a2">Also: ${list.slice(1).map(x => esc(x.t.short)).join(" &middot; ")}</span>` : "") : "";
  if (list.length && A.on && !ready()) html += `<span class="a2"><b>Tap the screen once to allow the alarm sound.</b></span>`;
  const L = $("alarmList"); if (L.innerHTML !== html) L.innerHTML = html;
  $("alarmSil").textContent = sil && !breaking ? `Silenced ${Math.ceil((A.silUntil - now) / 1000)} s - undo` : "Silence 2 min";
  const nl = document.querySelector('.tabs a[data-p="live"]'); if (nl) nl.classList.toggle("alarm", list.length > 0);
  const hc = document.querySelector("article.vital.heart"), rc = document.querySelector("article.vital.resp");
  if (hc) hc.classList.toggle("alarming", A.act.has("heart") || A.act.has("cardiac"));
  if (rc) rc.classList.toggle("alarming", A.act.has("breath") || A.act.has("apnea"));
  const st = $("alState");
  if (st) {
    const txt = !A.on ? "Alarm sound is OFF for this page. It switches on again when the page is opened again." :
      ready() ? "Sound is ready." : "Sound is blocked by the browser until you tap the page once.";
    if (st.textContent !== txt) { st.textContent = txt; st.className = "hint " + (A.on && ready() ? "okt" : "badt"); }
  }
  const li = $("alLite"); if (li) li.hidden = A.full;
}
function syncControls() {
  const b = $("btnSound");
  if (b) { b.setAttribute("aria-pressed", String(A.on)); const s = b.querySelector("span"); if (s) s.textContent = A.on ? "Alarms on" : "Alarms off"; }
  if ($("alOn")) $("alOn").checked = A.on;
  if ($("alVol")) { $("alVol").value = Math.round(A.vol * 100); $("alVolTxt").textContent = Math.round(A.vol * 100) + " %"; }
  if ($("alVib")) $("alVib").checked = A.vib;
  document.querySelectorAll("#alTab input[data-k]").forEach(i => { i.checked = enabled(i.dataset.k); i.disabled = !!BYK[i.dataset.k].lock; });
}
function buildTable() {
  const tb = $("alTab"); if (!tb) return;
  tb.tHead.rows[0].innerHTML = "<th>On</th><th>Alarm, when it starts, and its sound</th><th></th>";
  tb.tBodies[0].innerHTML = TYPES.map(t => `<tr><td><input type="checkbox" data-k="${t.k}" aria-label="${t.name}"${t.lock ? ' title="This safety alarm cannot be switched off"' : ""}></td>` +
    `<td><b>${t.name}</b><span class="pr p${t.prio}">${PN[t.prio]}</span>${t.lock ? '<span class="pr lk">always on</span>' : ""}${t.lic ? '<span class="pr lk">licence</span>' : ""}` +
    `<small>${t.when}</small><small class="snd">Sound: ${t.desc}</small></td>` +
    `<td><button class="btn sm" type="button" data-test="${t.k}">Test</button></td></tr>`).join("");
}
buildTable(); syncControls();
document.addEventListener("click", e => {
  const t = e.target.closest && e.target.closest("[data-test]");
  if (t) { ctx(); setTimeout(() => play(t.dataset.test, true), ready() ? 0 : 150); }
});
document.addEventListener("change", e => {
  const i = e.target;
  if (i.dataset && i.dataset.k && A.en.hasOwnProperty(i.dataset.k) && !BYK[i.dataset.k].lock) { A.en[i.dataset.k] = i.checked; save(); render(Date.now()); }
});
if ($("alOn")) $("alOn").addEventListener("change", e => { A.on = e.target.checked; syncControls(); render(Date.now()); });
if ($("alVol")) $("alVol").addEventListener("input", e => { A.vol = (+e.target.value) / 100; $("alVolTxt").textContent = e.target.value + " %"; save(); });
if ($("alVol")) $("alVol").addEventListener("change", () => play("heart", true));
if ($("alVib")) $("alVib").addEventListener("change", e => { A.vib = e.target.checked; save(); });
if ($("btnSound")) $("btnSound").addEventListener("click", () => { A.on = !A.on; syncControls(); if (A.on) { ctx(); setTimeout(() => play("heart", true), 150); } render(Date.now()); });
function silenceAll(now) { A.silUntil = now + 120000; for (const [k, a] of A.act) A.sil.set(k, Math.max(a.p, A.sil.get(k) || 0)); }
if ($("alarmX")) $("alarmX").addEventListener("click", () => {   // close the bar = silence the current alarms for 2 min and hide the bar
  const now = Date.now(); silenceAll(now); A.hideUntil = A.silUntil; render(now);
});
if ($("alarmSil")) $("alarmSil").addEventListener("click", () => {
  const now = Date.now();
  const breaking = silenced(now) && Array.from(A.act).some(([k, a]) => enabled(k) && mayBreak(k, a.p));
  if (silenced(now) && !breaking) { A.silUntil = 0; A.hideUntil = 0; A.sil.clear(); for (const k in A.last) A.last[k] = 0; }   // undo
  else { silenceAll(now); A.hideUntil = 0; }              // silence (also a new alarm that broke through); the bar stays visible
  render(now);
});
setInterval(tick, 500);
/* v1.3: active alarms, most urgent first (read by the status hero); userStop = this page pressed Stop on purpose */
function active() {
  return TYPES.filter(t => A.act.has(t.k)).map(t => { const a = A.act.get(t.k); return {k: t.k, name: t.name, short: t.short, p: a.p, t: a.t, since: a.since}; })
    .sort((x, y) => y.p - x.p || BYK[x.k].i - BYK[y.k].i);
}
function userStop() { A.userStopAt = Date.now(); A.wasMeasuring = false; }
window.CRAlarm = {update, tick, play, types: TYPES, state: A, conditions, events: () => A.log.slice(), active, userStop, setLimits, defLimits};
})();
