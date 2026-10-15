"use strict";
/* ============================ CardioResp UI ============================ */
(() => {
const $ = id => document.getElementById(id);
const host = location.hostname || "192.168.4.1";
let wsPort = 81;
const DN = 1024, RN = 3200;           // client rings: 10.24 s at 100 Hz, 320 s at 10 Hz
const st = {
  sid:null, info:null, S:null, spec:null, frozen:false, win:60, 
  D:{a:new Int16Array(DN), g:new Int16Array(DN), have:new Int32Array(DN).fill(-1), head:0},
  R:{v:new Float32Array(RN), m:new Uint8Array(RN), have:new Int32Array(RN).fill(-1), head:0},
  beats:[], breaths:[], trend:[], log:null, report:null, captured:null,
  scA:5, scG:0.3, scR:0.5, lastAna:0, lastMsg:0, connected:false, alarmT:0
};
let pal = {};
st.zc = {h:"", r:""};      // v1.2: trace colours when a value is outside its range (set by ext.js; values are never hidden)
function readPalette() {
  const cs = getComputedStyle(document.documentElement);
  for (const k of ["paper", "panel", "grid1", "grid2", "ink", "muted", "line", "heart", "breath", "gyro", "ok", "warn", "alert", "heart-soft", "breath-soft"]) pal[k] = cs.getPropertyValue("--" + k).trim();
  gridCache.clear();
}

/* --------------------------- ring buffers --------------------------- */
function resetBuffers() {
  st.D.have.fill(-1); st.D.head = 0;
  st.R.have.fill(-1); st.R.head = 0;
  st.beats = []; st.breaths = [];
}
function checkSid(sid) {
  if (sid === undefined || sid === null) return;
  if (st.sid !== null && sid !== st.sid) resetBuffers();
  st.sid = sid;
}
function onWave(m) {
  checkSid(m.sid);
  const D = st.D;
  for (let k = 0; k < m.a.length; k++) {
    const i = m.i + k, j = i % DN;
    D.a[j] = m.a[k]; D.g[j] = m.g[k]; D.have[j] = i;
    if (i + 1 > D.head) D.head = i + 1;
  }
  const R = st.R;
  for (let k = 0; k < m.r.length; k++) {
    const v = m.r[k], i = m.ri + k, j = i % RN;
    R.v[j] = Math.trunc(v) / 1000; R.m[j] = Math.abs(v % 1) > 0.25 ? 1 : 0; R.have[j] = i;
    if (i + 1 > R.head) R.head = i + 1;
  }
  const lastI = st.beats.length ? st.beats[st.beats.length - 1].i : -1;
  for (const b of m.b || []) {
    if (b[0] <= lastI) continue;
    st.beats.push({i:b[0], ibi:b[1], fl:b[2]});
    if (D.head - b[0] < 150) flashPulse();
  }
  if (st.beats.length > 900) st.beats.splice(0, st.beats.length - 900);
  const lastB = st.breaths.length ? st.breaths[st.breaths.length - 1].n : -1;
  for (const b of m.br || []) {
    if (b[0] <= lastB) continue;
    st.breaths.push({n:b[0], per:b[1], ti:b[2], te:b[3], amp:b[4] / 1000, fl:b[5], pk:b[6]});
  }
  if (st.breaths.length > 300) st.breaths.splice(0, st.breaths.length - 300);
}
let pulseT = 0;
function flashPulse() {
  const p = $("pulse"), h = simpleOn ? $("sHb") : null;
  p.classList.add("beat"); if (h) h.classList.add("beat");
  clearTimeout(pulseT);
  pulseT = setTimeout(() => { p.classList.remove("beat"); if (h) h.classList.remove("beat"); }, 110);
}

/* ----------------------------- connection ----------------------------- */
let ws = null, tries = 0, pollT = null, reconT = null;
function setConn(txt, cls) { $("connTxt").textContent = txt; $("connDot").className = "dot " + (cls || ""); }
function connect() {
  clearTimeout(reconT);
  setConn(tries ? "Reconnecting" : "Connecting", tries ? "bad" : "");
  try { ws = new WebSocket(`ws://${host}:${wsPort}/`); }
  catch (e) { schedule(); return; }
  ws.onopen = () => { tries = 0; st.connected = true; setConn("Live", "on"); stopPoll(); if (window.CR && CR.onOpen) CR.onOpen(); };
  ws.onmessage = ev => {
    st.lastMsg = performance.now();
    let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.t === "w") onWave(m);
    else if (m.t === "s") onStatus(m);
    else if (m.t === "f") { st.spec = m; drawSpectra(); }
    else if (m.t === "i") onInfo(m);
    else if (m.t === "a") onAcal(m);
    else if ((m.t === "v" || m.t === "m") && window.CR && CR.onMsg) CR.onMsg(m);
  };
  ws.onclose = () => { st.connected = false; ws = null; schedule(); };
  ws.onerror = () => { try { ws && ws.close(); } catch (e) {} };
}
function schedule() {
  tries++;
  setConn("Reconnecting", "bad");
  if (tries >= 3) startPoll();
  clearTimeout(reconT);
  reconT = setTimeout(connect, Math.min(5000, 700 * tries));
}
function startPoll() {
  if (pollT) return;
  pollT = setInterval(async () => {
    try { const r = await fetch("/api/status", {cache:"no-store"}); if (r.ok) { onStatus(await r.json()); setConn("Status only (no live stream)", "bad"); } } catch (e) {}
  }, 2000);
}
function stopPoll() { if (pollT) { clearInterval(pollT); pollT = null; } }
function send(o) { if (ws && ws.readyState === 1) { ws.send(JSON.stringify(o)); return true; } return false; }
setInterval(() => { if (st.connected && performance.now() - st.lastMsg > 7000 && ws) { try { ws.close(); } catch (e) {} } }, 2000);
// v1.4: free the monitor's connection slot at once on reload / leaving, so the reloaded page connects immediately
window.addEventListener("pagehide", () => { clearTimeout(reconT); stopPoll(); if (ws) { ws.onclose = null; try { ws.close(); } catch (e) {} ws = null; } });
window.addEventListener("pageshow", ev => { if (ev.persisted && !ws) { tries = 0; connect(); } });

/* ------------------------------ canvases ------------------------------ */
function fit(c) {
  const r = c.getBoundingClientRect(), d = Math.min(window.devicePixelRatio || 1, 2.5);
  const w = Math.max(1, Math.round(r.width * d)), h = Math.max(1, Math.round(r.height * d));
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext("2d");
  g.setTransform(d, 0, 0, d, 0, 0);
  return {g, W:r.width, H:r.height};
}
const gridCache = new Map();
function paper(g, W, H, pxMinorX, majorEvery, pxMinorY) {
  if (!(W >= 2 && H >= 2)) return;            // v1.2: canvas on a hidden page
  const key = [W, H, pxMinorX, majorEvery, pxMinorY].map(v => Math.round(v * 10)).join(":");
  let off = gridCache.get(key);
  if (!off) {
    const d = Math.min(window.devicePixelRatio || 1, 2.5);
    off = document.createElement("canvas");
    off.width = Math.round(W * d); off.height = Math.round(H * d);
    const o = off.getContext("2d");
    o.setTransform(d, 0, 0, d, 0, 0);
    o.fillStyle = pal.paper; o.fillRect(0, 0, W, H);
    const lines = (n, step, horiz) => {
      for (let k = 0; k <= n; k++) {
        const p = Math.round(k * step) + 0.5, major = k % majorEvery === 0;
        o.strokeStyle = major ? pal.grid2 : pal.grid1; o.lineWidth = major ? 1 : 0.6;
        o.beginPath();
        if (horiz) { o.moveTo(0, p); o.lineTo(W, p); } else { o.moveTo(p, 0); o.lineTo(p, H); }
        o.stroke();
      }
    };
    if (pxMinorX >= 3) lines(Math.ceil(W / pxMinorX), pxMinorX, false);
    else lines(Math.ceil(W / (pxMinorX * majorEvery)), pxMinorX * majorEvery, false);
    lines(Math.ceil(H / pxMinorY), pxMinorY, true);
    if (gridCache.size > 12) gridCache.clear();
    gridCache.set(key, off);
  }
  g.drawImage(off, 0, 0, W, H);
}
function label(g, x, y, txt, col, align, size) {
  g.font = `${size || 11}px system-ui,sans-serif`;
  g.textAlign = align || "left"; g.textBaseline = "alphabetic";
  g.fillStyle = col || pal.muted;
  g.fillText(txt, x, y);
}
function robustMax(vals) {
  let m = 0;
  for (let i = 0; i < vals.length; i++) { const v = Math.abs(vals[i]); if (v > m) m = v; }
  return m;                                                           // v1.2: true peak (no percentile trimming)
}
function smoothScale(cur, target, min) {
  target = Math.max(min, target * 1.1);
  return target > cur ? target : cur + 0.02 * (target - cur);       // v1.2: grows at once, so large (abnormal) signals are never clipped
}

/* heart strip: classic monitor sweep, 5 s, SCG on top, GCG below */
function drawHeart() {
  const c = $("cvHeart"), {g, W, H} = fit(c);
  const N = 500, pxs = W / 5;
  paper(g, W, H, pxs * 0.04, 5, pxs * 0.04 < 3 ? H / 20 : pxs * 0.04);
  const D = st.D, head = D.head;
  if (!head) { label(g, W / 2, H / 2, "waiting for data", pal.muted, "center", 13); return; }
  const gap = 14, start = Math.max(0, head - N + gap);
  const va = [], vg = [];
  for (let i = start; i < head; i++) { const j = i % DN; if (D.have[j] === i) { va.push(D.a[j] / 100); vg.push(D.g[j] / 1000); } }
  st.scA = smoothScale(st.scA, robustMax(va), 0.5);
  st.scG = smoothScale(st.scG, robustMax(vg), 0.02);
  const yA = H * 0.36, hA = H * 0.30, yG = H * 0.80, hG = H * 0.15;
  const X = i => (i % N) / N * W;
  const trace = (arr, sc, y0, hh, col, w) => {
    g.strokeStyle = col; g.lineWidth = w; g.lineJoin = "round"; g.beginPath();
    let px = -1, pen = false;
    for (let i = start; i < head; i++) {
      const j = i % DN;
      if (D.have[j] !== i) { pen = false; continue; }
      const x = X(i), y = y0 - (arr(j) / sc) * hh;
      if (!pen || x < px) g.moveTo(x, y); else g.lineTo(x, y);
      px = x; pen = true;
    }
    g.stroke();
  };
  trace(j => D.a[j] / 100, st.scA, yA, hA, st.zc.h || pal.heart, 1.4);
  trace(j => D.g[j] / 1000, st.scG, yG, hG, pal.gyro, 1.1);
  // sweep cursor
  const cx = X(head);
  g.fillStyle = pal.paper; g.globalAlpha = 0.85; g.fillRect(cx, 0, gap / N * W, H); g.globalAlpha = 1;
  g.fillStyle = pal.ink; g.fillRect(cx - 1, 0, 2, H);
  // beat markers with interval (ms)
  const S = st.S;
  if (S && S.pres) {
    for (const b of st.beats) {
      if (b.i < start || b.i >= head) continue;
      const x = X(b.i);
      g.fillStyle = (b.fl & 1) ? pal.heart : pal.muted;
      g.beginPath(); g.moveTo(x - 4, 3); g.lineTo(x + 4, 3); g.lineTo(x, 10); g.fill();
      if (b.ibi) label(g, x + 5, 12, String(b.ibi), (b.fl & 1) ? pal.ink : pal.muted, "left", 10);
    }
  }
  label(g, 6, H * 0.08 + 12, `SCG ±${st.scA.toFixed(st.scA < 10 ? 1 : 0)} mg`, pal.heart, "left", 10);
  label(g, 6, H * 0.63 + 4, `GCG ±${st.scG.toFixed(2)} °/s`, pal.gyro, "left", 10);
  if (S && S.mot) { g.fillStyle = pal.warn; label(g, W - 8, H - 8, "movement (signal shown, not used for rate)", pal.warn, "right", 11); }
}

/* breathing strip: scrolling, 60 s or 5 min */
function drawResp() {
  const c = $("cvResp"), {g, W, H} = fit(c);
  const win = st.win, N = win * 10, pxs = W / win;
  paper(g, W, H, win === 60 ? pxs : pxs * 5, win === 60 ? 5 : 6, H / 10);
  const R = st.R, head = R.head;
  if (!head) { label(g, W / 2, H / 2, "waiting for data", pal.muted, "center", 13); return; }
  const start = Math.max(0, head - N);
  const X = i => W - (head - 1 - i) / N * W;
  const vals = [];
  for (let i = start; i < head; i++) { const j = i % RN; if (R.have[j] === i) vals.push(R.v[j]); }
  st.scR = smoothScale(st.scR, robustMax(vals), 0.08);
  const y0 = H * 0.52, hh = H * 0.40, Y = v => y0 - (v / st.scR) * hh;
  // movement shading
  g.fillStyle = pal.line;
  for (let i = start; i < head;) {
    const j = i % RN;
    if (!(R.have[j] === i && R.m[j])) { i++; continue; }
    const s = i; while (i < head && R.have[i % RN] === i && R.m[i % RN]) i++;
    g.globalAlpha = 0.35; g.fillRect(X(s), 0, Math.max(1, X(i) - X(s)), H); g.globalAlpha = 1;      // v1.2: light tint only; the trace stays visible
  }
  // ongoing pause
  const S = st.S;
  if (S && S.apc > 0) { g.fillStyle = pal["heart-soft"]; g.fillRect(X(head - S.apc * 10), 0, W, H); }
  // trace (min/max per pixel column)
  // v1.2: every sample is drawn. Samples taken during movement are drawn in the warning colour instead of being hidden.
  g.lineWidth = 1.6; g.lineJoin = "round";
  const colN = st.zc.r || pal.breath, colM = pal.warn;
  const per = Math.max(1, (head - start) / W);
  if (per <= 1.5) {
    let pen = false, curM = -1;
    for (let i = start; i < head; i++) {
      const j = i % RN;
      if (R.have[j] !== i) { if (pen) g.stroke(); pen = false; continue; }
      const x = X(i), y = Y(R.v[j]), mv = R.m[j] ? 1 : 0;
      if (pen && mv !== curM) { g.lineTo(x, y); g.stroke(); pen = false; }          // close the run, continue from this point in the new colour
      if (!pen) { g.strokeStyle = mv ? colM : colN; g.beginPath(); g.moveTo(x, y); pen = true; curM = mv; }
      else g.lineTo(x, y);
    }
    if (pen) g.stroke();
  } else {
    for (const pass of [0, 1]) {
      g.strokeStyle = pass ? colM : colN; g.beginPath();
      let pen = false;
      for (let b = start; b < head; b += per) {
        let mn = Infinity, mx = -Infinity, first = null, mot = 0, cnt = 0;
        for (let i = Math.floor(b); i < Math.min(head, Math.floor(b + per)); i++) {
          const j = i % RN;
          if (R.have[j] !== i) continue;
          cnt++; if (R.m[j]) mot++;
          if (first === null) first = R.v[j];
          mn = Math.min(mn, R.v[j]); mx = Math.max(mx, R.v[j]);
        }
        if (first === null || (pass === 1 && mot * 2 < cnt) || (pass === 0 && mot * 2 >= cnt && false)) { pen = false; continue; }
        const x = X(b);
        if (!pen) g.moveTo(x, Y(first)); else g.lineTo(x, Y(first));
        g.lineTo(x, Y(mn)); g.lineTo(x, Y(mx));
        pen = true;
      }
      g.stroke();
    }
  }
  // breath markers: end-inspiration peak with the breath rate
  for (const b of (win === 60 ? st.breaths : [])) {
    if (b.pk < start || b.pk >= head || (b.fl & 2)) continue;   // no markers for breaths spoiled by movement
    const j = b.pk % RN;
    if (R.have[j] !== b.pk) continue;
    const x = X(b.pk), y = Y(R.v[j]);
    g.fillStyle = (b.fl & 1) ? pal.breath : pal.muted;
    g.beginPath(); g.arc(x, y, 2.6, 0, 6.3); g.fill();
    if (win === 60 && (b.fl & 1) && b.per) label(g, x, Math.max(11, y - 7), (60000 / b.per).toFixed(0), pal.muted, "center", 10);
  }
  // time axis
  const step = win === 60 ? 10 : 60;
  for (let s2 = step; s2 < win; s2 += step) label(g, W - s2 * pxs, H - 4, `-${s2 < 60 ? s2 + " s" : s2 / 60 + " min"}`, pal.muted, "center", 10);
  label(g, 6, 12, `in ↑  ±${st.scR.toFixed(2)}°`, pal.breath, "left", 10);
}

/* generic line chart helpers */
function axisFrame(g, W, H, l, r, t, b) { g.strokeStyle = pal.line; g.lineWidth = 1; g.strokeRect(l + 0.5, t + 0.5, W - l - r, H - t - b); }
function drawSpecChart(id, arr, df, mult, xmax, col, peak, ticks, unit) {
  const c = $(id), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  const l = 6, r = 6, t = 8, b = 20, cw = W - l - r, ch = H - t - b;
  axisFrame(g, W, H, l, r, t, b);
  if (!arr || !arr.length || !df) { label(g, W / 2, H / 2, "collecting data", pal.muted, "center", 12); return; }
  g.beginPath();
  g.moveTo(l, t + ch);
  for (let i = 0; i < arr.length; i++) {
    const fx = i * df * mult; if (fx > xmax) break;
    g.lineTo(l + fx / xmax * cw, t + ch - arr[i] / 1000 * ch);
  }
  g.lineTo(l + Math.min(xmax, (arr.length - 1) * df * mult) / xmax * cw, t + ch);
  g.closePath();
  g.fillStyle = col; g.globalAlpha = 0.14; g.fill(); g.globalAlpha = 1;
  g.strokeStyle = col; g.lineWidth = 1.4; g.stroke();
  for (const tk of ticks) label(g, l + tk / xmax * cw, H - 5, String(tk), pal.muted, tk === 0 ? "left" : tk === xmax ? "right" : "center", 10);
  if (peak > 0 && peak < xmax) {
    const x = l + peak / xmax * cw;
    g.setLineDash([3, 3]); g.strokeStyle = pal.ink; g.beginPath(); g.moveTo(x, t); g.lineTo(x, t + ch); g.stroke(); g.setLineDash([]);
    label(g, Math.min(x + 4, W - 60), t + 12, `${peak.toFixed(1)} ${unit}`, pal.ink, "left", 11);
  }
}
function drawSpectra() {
  const s = st.spec; if (!s) return;
  drawSpecChart("cvSpecC", s.c, s.cd, 60, 240, pal.heart, s.cp * 60, [0, 60, 120, 180, 240], "bpm");
  drawSpecChart("cvSpecR", s.r, s.rd, 60, 60, pal.breath, s.rp * 60, [0, 15, 30, 45, 60], "/min");
  $("specCTxt").textContent = s.cp > 0 ? `Peak at ${(s.cp * 60).toFixed(1)} beats/min. Smaller peaks at multiples are harmonics of the same rhythm.` : "Spectrum of the fused beat envelope over 60 s.";
  $("specRTxt").textContent = s.rp > 0 ? `Dominant breathing frequency ${(s.rp * 60).toFixed(1)} breaths/min (${s.rp.toFixed(3)} Hz).` : "Welch spectrum of the chest-tilt signal over 5 minutes.";
  // vibration spectrum in dB
  const c = $("cvSpecV"), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  const l = 26, r = 6, t = 8, b = 20, cw = W - l - r, ch = H - t - b;
  g.fillStyle = pal["heart-soft"]; g.fillRect(l + 5 / 50 * cw, t, 25 / 50 * cw, ch);
  axisFrame(g, W, H, l, r, t, b);
  const line = (arr, col) => {
    if (!arr) return;
    g.strokeStyle = col; g.lineWidth = 1.3; g.beginPath();
    arr.forEach((v, i) => { const x = l + i * s.vd / 50 * cw, y = t + (-v / 60) * ch; i ? g.lineTo(x, y) : g.moveTo(x, y); });
    g.stroke();
  };
  line(s.va, pal.heart); line(s.vg, pal.gyro);
  for (const tk of [0, 10, 20, 30, 40, 50]) label(g, l + tk / 50 * cw, H - 5, String(tk), pal.muted, tk === 50 ? "right" : "center", 10);
  for (const db of [0, -20, -40, -60]) label(g, l - 3, t + (-db / 60) * ch + 4, String(db), pal.muted, "right", 9);
  label(g, W - r - 4, t + ch - 4, "Hz", pal.muted, "right", 10);
}
function drawLevel(S) {
  const c = $("cvLevel"), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  if (W < 30 || H < 30) return;        // v1.2: hidden page
  const cx = W / 2, cy = H / 2, R = Math.min(W, H) / 2 - 8;
  g.fillStyle = pal.panel; g.beginPath(); g.arc(cx, cy, R, 0, 6.3); g.fill();
  g.strokeStyle = pal.line; g.lineWidth = 1.2;
  for (const f2 of [1, 0.5]) { g.beginPath(); g.arc(cx, cy, R * f2, 0, 6.3); g.stroke(); }
  g.beginPath(); g.moveTo(cx - R, cy); g.lineTo(cx + R, cy); g.moveTo(cx, cy - R); g.lineTo(cx, cy + R); g.stroke();
  label(g, cx + R - 2, cy - 4, "+X", pal.muted, "right", 10); label(g, cx + 4, cy - R + 11, "+Y head", pal.muted, "left", 10);
  const ok = v => Array.isArray(v) && v.length === 3 && v.every(isNum);
  // v1.3: the gravity reference of the last calibration (ring), so a change of posture is visible at a glance
  const g0 = st.info && st.info.calib && ok(st.info.calib.g0) && Math.hypot(...st.info.calib.g0) > 0.5 ? st.info.calib.g0 : null;
  if (g0) {
    g.strokeStyle = pal.muted; g.lineWidth = 1.5; g.setLineDash([3, 3]);
    g.beginPath(); g.arc(cx + g0[0] * R, cy - g0[1] * R, 12, 0, 6.3); g.stroke(); g.setLineDash([]);
  }
  if (!S || !ok(S.g)) { label(g, cx, cy + 4, "no sensor data", pal.muted, "center", 11); return; }
  const gx = S.g[0], gy = S.g[1], gz = S.g[2];
  const x = cx + gx * R, y = cy - gy * R;
  g.fillStyle = gz >= 0 ? pal.breath : pal.heart;
  g.globalAlpha = S.mot ? 0.55 : 1;
  g.beginPath(); g.arc(x, y, 9, 0, 6.3); g.fill(); g.globalAlpha = 1;
  label(g, cx, cy + R * 0.74, gz >= 0 ? "Z up" : "Z down", pal.muted, "center", 10);      // inside the round view (the canvas is clipped to a circle)
}
/* v1.3: posture in plain words from the gravity direction (sensor fixed with Y towards the head, Z out of the chest) */
function postureOf(gv) {
  if (!Array.isArray(gv) || !gv.every(isNum)) return null;
  const [x, y, z] = gv;
  if (z >= 0.8) return ["ok", "Lying on the back"];
  if (y >= 0.7) return ["ok", "Sitting or standing upright"];
  if (y >= 0.3 && z >= 0.3) return ["ok", "Propped up / reclining"];
  if (Math.abs(x) >= 0.7) return ["ok", "Lying on the side"];
  if (z <= -0.7) return ["bad", "Face down, or the sensor is upside down (Z must face out of the chest)"];
  if (y <= -0.6) return ["bad", "Sensor upside down? Y must point towards the head"];
  return ["ok", "Tilted position"];
}
function placementChecks(S, measuring) {
  const out = [];
  const has = S && Array.isArray(S.g) && S.g.every(isNum);
  if (!has) { out.push(["no", S && S.cal === 3 ? "Sensor not connected" : "Waiting for the sensor"]); return out; }
  const p = postureOf(S.g); if (p) out.push(p);
  out.push(S.mot ? ["bad", measuring ? "Moving now" : "Moving - keep still before Start"] : ["ok", "Still"]);
  if (measuring && S.post) out.push(["bad", "Posture changed since the start - press Stop, then Start, to recalibrate"]);
  else if (isNum(S.tilt) && S.tilt > 30 && !measuring && (S.cal === 4)) out.push(["bad", `Turned ${Math.round(S.tilt)}° from the last start position`]);
  return out;
}
/* v1.3: placement and gravity are drawn in EVERY state: before Start, while calibrating, while measuring, with or without a licence */
let orientWhere = "";
function drawOrient(S) {
  const has = !!S && Array.isArray(S.g) && S.g.every(isNum);
  const measuring = !!S && S.cal === 2;
  $("oRP").textContent = has ? `${f(S.roll, 0)}° / ${f(S.pitch, 0)}°` : "--";
  $("oUp").textContent = has ? axisUp(S.g) : "--";
  $("oTilt").textContent = !has ? "--" : isNum(S.tilt) ? `${f(S.tilt, 0)}°${S.post ? " - posture changed" : ""}` : "no calibration yet";
  $("oMot").textContent = !has ? "--" : S.mot ? "moving" : isNum(S.mf) ? `still (${Math.round((1 - S.mf) * 100)} % of the last minute)` : isNum(S.still) && S.still >= 0 ? `still (steadiness ${S.still} %)` : "still";
  $("oSrc").textContent = !has ? "" : measuring ? "while measuring" : "live preview, not measuring";
  const ch = placementChecks(S, measuring);
  const html = ch.map(([c, t]) => `<li class="${c}">${esc(t)}</li>`).join("");
  if ($("oChecks").innerHTML !== html) $("oChecks").innerHTML = html;
  drawLevel(has ? S : null);
  if (S && S.vc) drawAxes(S);
  // while measuring the vital signs come first and the placement card moves below the assessment
  const where = measuring ? "below" : "top";
  if (where !== orientWhere) {
    orientWhere = where;
    const o = $("orientSec"), anchor = where === "top" ? $("hero") : $("sevCard");
    if (o && anchor && anchor.parentNode) anchor.parentNode.insertBefore(o, anchor.nextSibling);
    setTimeout(() => drawLevel(st.S && Array.isArray(st.S.g) ? st.S : null), 30);
  }
}
function drawAxes(S) {
  const c = $("cvAxes"), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  if (!S || !S.vc) return;
  const groups = [["Heart (accel)", S.vc, pal.heart], ["Heart (gyro)", S.vg, pal.gyro], ["Breathing", S.vr, pal.breath]];
  const gw = W / 3, bw = Math.min(18, gw / 5), base = H - 30, hh = H - 50;
  groups.forEach(([name, v, col], k) => {
    const x0 = k * gw + gw / 2 - 1.5 * bw - bw;
    ["X", "Y", "Z"].forEach((ax, i) => {
      const h = Math.abs(v[i]) * hh, x = x0 + i * (bw + bw / 2);
      g.fillStyle = col; g.globalAlpha = 0.25 + 0.75 * Math.abs(v[i]); g.fillRect(x, base - h, bw, h); g.globalAlpha = 1;
      label(g, x + bw / 2, base + 12, ax, pal.muted, "center", 10);
    });
    label(g, k * gw + gw / 2, H - 2, name, col, "center", 10);
  });
}
function drawTrend() {
  const c = $("cvTrend"), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  const l = 30, r = 30, t = 8, b = 18, cw = W - l - r, ch = H - t - b;
  axisFrame(g, W, H, l, r, t, b);
  const T = st.trendWin ? st.trend.filter(p => p.t >= Date.now() - st.trendWin * 1000) : st.trend;
  if (T.length < 2) { label(g, W / 2, H / 2, "trend builds up during the session", pal.muted, "center", 12); return; }
  const P = ageProfile(st.S ? st.S.age : 30);
  const hrs = T.map(p => p.hr).filter(isNum), rrs = T.map(p => p.rr).filter(isNum);
  const hLo = Math.floor(Math.min(P.hr[0], ...hrs) / 10) * 10 - 10, hHi = Math.ceil(Math.max(P.hr[1], ...hrs) / 10) * 10 + 10;
  const rHi = Math.ceil(Math.max(P.rr[1] * 1.5, ...rrs) / 5) * 5;
  const t0 = T[0].t, t1 = T[T.length - 1].t, X = tt => l + (tt - t0) / Math.max(1, t1 - t0) * cw;
  g.fillStyle = pal["heart-soft"];
  g.fillRect(l, t + ch - (P.hr[1] - hLo) / (hHi - hLo) * ch, cw, (P.hr[1] - P.hr[0]) / (hHi - hLo) * ch);
  const series = (key, lo, hi, col) => {
    g.strokeStyle = col; g.lineWidth = 1.4; g.beginPath();
    let pen = false;
    const stp = Math.max(1, Math.floor(T.length / cw));
    for (let i = 0; i < T.length; i += stp) {
      const v = T[i][key];
      if (!isNum(v)) { pen = false; continue; }
      const x = X(T[i].t), y = t + ch - (v - lo) / (hi - lo) * ch;
      pen ? g.lineTo(x, y) : g.moveTo(x, y); pen = true;
    }
    g.stroke();
  };
  series("hr", hLo, hHi, pal.heart);
  series("rr", 0, rHi, pal.breath);
  for (let k = 0; k <= 4; k++) {
    const y = t + ch - k / 4 * ch;
    label(g, l - 4, y + 4, (hLo + k / 4 * (hHi - hLo)).toFixed(0), pal.heart, "right", 10);
    label(g, W - r + 4, y + 4, (k / 4 * rHi).toFixed(0), pal.breath, "left", 10);
  }
  const mins = (t1 - t0) / 60000;
  label(g, l, H - 3, st.trendWin && T[0].t > st.trend[0].t ? `last ${Math.round(st.trendWin / 60)} min` : "session start", pal.muted, "left", 10);
  label(g, W - r, H - 3, `${mins.toFixed(mins < 10 ? 1 : 0)} min`, pal.muted, "right", 10);
}
function drawTachoPoinc() {
  const v = st.beats.filter(b => (b.fl & 1) && b.ibi > 0).slice(-300);
  let c = $("cvTacho"), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  const l = 34, r = 6, t = 8, b = 16, cw = W - l - r, ch = H - t - b;
  axisFrame(g, W, H, l, r, t, b);
  if (v.length < 3) { label(g, W / 2, H / 2, "waiting for beats", pal.muted, "center", 12); $("tachoTxt").textContent = "Tachogram of accepted intervals."; }
  else {
    const ib = v.map(x => x.ibi), lo = Math.floor(Math.min(...ib) / 50) * 50 - 50, hi = Math.ceil(Math.max(...ib) / 50) * 50 + 50;
    g.strokeStyle = pal.heart; g.lineWidth = 1.2; g.beginPath();
    v.forEach((x, i) => { const X = l + i / (v.length - 1) * cw, Y = t + ch - (x.ibi - lo) / (hi - lo) * ch; i ? g.lineTo(X, Y) : g.moveTo(X, Y); });
    g.stroke();
    for (let k = 0; k <= 2; k++) label(g, l - 4, t + ch - k / 2 * ch + 4, String(Math.round(lo + k / 2 * (hi - lo))), pal.muted, "right", 10);
    label(g, W - r, H - 3, `${v.length} beats`, pal.muted, "right", 10);
    const m = ib.reduce((a2, b2) => a2 + b2, 0) / ib.length;
    $("tachoTxt").textContent = `Mean interval ${m.toFixed(0)} ms (${(60000 / m).toFixed(0)} bpm), range ${Math.min(...ib)}-${Math.max(...ib)} ms. Waves following breathing are normal sinus arrhythmia.`;
  }
  ({g, W, H} = fit($("cvPoinc")));
  g.clearRect(0, 0, W, H);
  const s = Math.min(W - 40, H - 24), x0 = 34, y0 = 6;
  g.strokeStyle = pal.line; g.strokeRect(x0 + 0.5, y0 + 0.5, s, s);
  const pairs = [];
  for (let i = 1; i < st.beats.length; i++) {
    const a2 = st.beats[i - 1], b2 = st.beats[i];
    if ((a2.fl & 1) && (b2.fl & 1) && a2.ibi && b2.ibi) pairs.push([a2.ibi, b2.ibi]);
  }
  const pp = pairs.slice(-300);
  if (pp.length < 5) { label(g, x0 + s / 2, y0 + s / 2, "waiting for beats", pal.muted, "center", 12); return; }
  const all = pp.flat(), lo = Math.floor(Math.min(...all) / 50) * 50 - 50, hi = Math.ceil(Math.max(...all) / 50) * 50 + 50;
  g.setLineDash([3, 3]); g.beginPath(); g.moveTo(x0, y0 + s); g.lineTo(x0 + s, y0); g.stroke(); g.setLineDash([]);
  g.fillStyle = pal.gyro; g.globalAlpha = 0.7;
  for (const [a2, b2] of pp) { g.beginPath(); g.arc(x0 + (a2 - lo) / (hi - lo) * s, y0 + s - (b2 - lo) / (hi - lo) * s, 2.2, 0, 6.3); g.fill(); }
  g.globalAlpha = 1;
  label(g, x0 - 4, y0 + 10, String(hi), pal.muted, "right", 10); label(g, x0 - 4, y0 + s, String(lo), pal.muted, "right", 10);
  const d = pp.map(p => p[1] - p[0]);
  const sd1 = Math.sqrt(0.5) * sd(d), sdnn = sd(all);
  const sd2 = Math.sqrt(Math.max(0, 2 * sdnn * sdnn - sd1 * sd1));
  $("poincTxt").textContent = `SD1 ${sd1.toFixed(0)} ms (beat-to-beat), SD2 ${sd2.toFixed(0)} ms (longer-term). A tight, elongated cloud along the diagonal is typical of a regular sinus rhythm.`;
}
/* ------------------------- advanced analysis tools (v1.2 final) ------------------------- */
const anaOpen = () => { const d = $("anaDrop"); return !d || d.open; };
const meanOf = a => a.reduce((s, v) => s + v, 0) / (a.length || 1);
const sdOf = a => { if (a.length < 2) return NaN; const m = meanOf(a); return Math.sqrt(a.reduce((s, v) => s + (v - m) * (v - m), 0) / (a.length - 1)); };
/* average heartbeat: SCG and GCG cut around every accepted beat marker in the 10-s buffer and averaged */
function ensembleData() {
  const D = st.D, pre = 15, post = 60, out = {a: [], g: []};
  const lo = D.head - DN + pre, hi = D.head - post - 1;
  for (const b of st.beats) {
    if (!(b.fl & 1) || b.i < lo || b.i > hi) continue;
    const sa = [], sg = []; let ok = true;
    for (let k = -pre; k <= post; k++) { const i = b.i + k, j = ((i % DN) + DN) % DN; if (D.have[j] !== i) { ok = false; break; } sa.push(D.a[j] / 100); sg.push(D.g[j] / 1000); }
    if (ok) { out.a.push(sa); out.g.push(sg); }
  }
  return out;
}
function drawEnsemble() {
  const c = $("cvEns"); if (!c) return;
  const {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  if (W < 40 || H < 40) return;
  const E = ensembleData(), n = E.a.length, l = 8, r = 8, t = 6, b = 18, cw = W - l - r, half = (H - t - b) / 2;
  axisFrame(g, W, H, l, r, t, b);
  const N = 76, X = k => l + k / (N - 1) * cw, x0 = X(15);
  g.strokeStyle = pal.line; g.setLineDash([3, 3]); g.beginPath(); g.moveTo(x0, t); g.lineTo(x0, H - b); g.stroke(); g.setLineDash([]);
  for (const ms of [0, 200, 400, 600]) label(g, X(15 + ms / 10), H - 4, ms + " ms", pal.muted, "center", 10);
  if (n < 3) { label(g, W / 2, H / 2, "needs at least 3 clean beats in the last 10 s", pal.muted, "center", 12); $("ensTxt").textContent = "Averages the chest vibration of the recent beats, aligned on the beat marker."; return; }
  const draw = (set, y0, col, unit) => {
    const mean = Array.from({length: N}, (_, k) => meanOf(set.map(s => s[k])));
    const amp = Math.max(1e-6, ...set.flat().map(Math.abs)) * 1.05, Y = v => y0 + half / 2 - v / amp * (half / 2 - 4);
    g.globalAlpha = 0.18; g.strokeStyle = col; g.lineWidth = 1;
    for (const s of set.slice(-20)) { g.beginPath(); s.forEach((v, k) => k ? g.lineTo(X(k), Y(v)) : g.moveTo(X(k), Y(v))); g.stroke(); }
    g.globalAlpha = 1; g.lineWidth = 2.2; g.beginPath(); mean.forEach((v, k) => k ? g.lineTo(X(k), Y(v)) : g.moveTo(X(k), Y(v))); g.stroke();
    label(g, l + 4, y0 + 12, `${unit === "mg" ? "SCG" : "GCG"} ±${amp.toFixed(unit === "mg" ? 1 : 2)} ${unit}`, col, "left", 10);
    return mean;
  };
  const ma = draw(E.a, t, pal.heart, "mg"); draw(E.g, t + half, pal.gyro, "°/s");
  // main peak after the marker, peak-to-peak and beat-to-beat consistency
  let kp = 15; for (let k = 15; k < 15 + 25; k++) if (Math.abs(ma[k]) > Math.abs(ma[kp])) kp = k;
  const p2p = Math.max(...ma) - Math.min(...ma);
  const corr = s => { const ms = meanOf(s), mm = meanOf(ma); let num = 0, da = 0, db = 0; for (let k = 0; k < N; k++) { num += (s[k] - ms) * (ma[k] - mm); da += (s[k] - ms) ** 2; db += (ma[k] - mm) ** 2; } return num / Math.sqrt(da * db || 1); };
  const cons = meanOf(E.a.map(corr));
  $("ensTxt").textContent = `${n} beats averaged. Peak-to-peak ${p2p.toFixed(1)} mg; strongest deflection ${((kp - 15) * 10)} ms after the beat marker. ` +
    `Beat-to-beat consistency ${(cons * 100).toFixed(0)} % (${cons > 0.8 ? "very similar beats, good contact" : cons > 0.5 ? "moderate" : "beats differ: movement, weak contact or irregular rhythm"}).`;
}
/* beat-interval histogram and heart-rate variability statistics (last 300 accepted beats) */
function hrvStats() {
  const v = st.beats.filter(b => (b.fl & 1) && b.ibi > 0).slice(-300), ib = v.map(b => b.ibi), d = [];
  for (let i = 1; i < st.beats.length; i++) { const a = st.beats[i - 1], b = st.beats[i]; if ((a.fl & 1) && (b.fl & 1) && a.ibi && b.ibi) d.push(b.ibi - a.ibi); }
  const dd = d.slice(-300), m = meanOf(ib), sdnn = sdOf(ib), rmssd = dd.length >= 2 ? Math.sqrt(meanOf(dd.map(x => x * x))) : NaN;
  const sd1 = Math.SQRT1_2 * sdOf(dd), sd2 = Math.sqrt(Math.max(0, 2 * sdnn * sdnn - sd1 * sd1));
  return {ib, n: ib.length, m, sdnn, rmssd, pnn50: dd.length ? 100 * dd.filter(x => Math.abs(x) > 50).length / dd.length : NaN, sd1, sd2, cv: 100 * sdnn / m};
}
function drawHist() {
  const c = $("cvHist"); if (!c) return;
  const {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  if (W < 40 || H < 40) return;
  const s = hrvStats(), l = 8, r = 8, t = 6, b = 18, cw = W - l - r, ch = H - t - b;
  axisFrame(g, W, H, l, r, t, b);
  if (s.n < 10) { label(g, W / 2, H / 2, "needs at least 10 accepted beats", pal.muted, "center", 12); $("hrvStats").innerHTML = ""; return; }
  const lo = Math.floor((Math.min(...s.ib) - 20) / 20) * 20, hi = Math.ceil((Math.max(...s.ib) + 20) / 20) * 20, nb = Math.max(1, (hi - lo) / 20);
  const cnt = new Array(nb).fill(0); s.ib.forEach(x => { cnt[Math.min(nb - 1, Math.floor((x - lo) / 20))]++; });
  const mx = Math.max(...cnt), bw = cw / nb;
  cnt.forEach((k, i) => { const h = k / mx * (ch - 12); g.fillStyle = pal.heart; g.globalAlpha = 0.75; g.fillRect(l + i * bw + 1, t + ch - h, Math.max(1, bw - 2), h); });
  g.globalAlpha = 1;
  const xm = l + (s.m - lo) / (hi - lo) * cw; g.strokeStyle = pal.ink; g.setLineDash([3, 3]); g.beginPath(); g.moveTo(xm, t); g.lineTo(xm, t + ch); g.stroke(); g.setLineDash([]);
  label(g, l, H - 4, lo + " ms", pal.muted, "left", 10); label(g, W - r, H - 4, hi + " ms", pal.muted, "right", 10); label(g, xm, t + 10, "mean", pal.ink, "center", 10);
  const f0 = (v, d) => isNum(v) ? v.toFixed(d || 0) : "--";
  $("hrvStats").innerHTML = kvHtml([["Beats", s.n], ["Mean interval", `${f0(s.m)} ms (${f0(60000 / s.m)} bpm)`], ["SDNN / RMSSD", `${f0(s.sdnn)} / ${f0(s.rmssd)} ms`],
    ["pNN50", `${f0(s.pnn50, 1)} %`], ["SD1 / SD2 (Poincare)", `${f0(s.sd1)} / ${f0(s.sd2)} ms`], ["Variation (CV)", `${f0(s.cv, 1)} %`]]);
}
/* breath by breath: rate of every breath (bars) and its depth (dots) */
function drawBreathByBreath() {
  const c = $("cvBreaths"); if (!c) return;
  const {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  if (W < 40 || H < 40) return;
  const B = st.breaths.filter(b => b.per > 0 && (b.fl & 1)).slice(-40), l = 30, r = 30, t = 8, b = 18, cw = W - l - r, ch = H - t - b;
  axisFrame(g, W, H, l, r, t, b);
  if (B.length < 3) { label(g, W / 2, H / 2, "waiting for breaths", pal.muted, "center", 12); $("bbbTxt").textContent = "Rate (bars) and depth (dots) of each recent breath."; return; }
  const rates = B.map(x => 60000 / x.per), amps = B.map(x => x.amp), rMax = Math.ceil(Math.max(...rates, 20) / 5) * 5, aMax = Math.max(...amps) * 1.15 || 1;
  const P = ageProfile(st.S ? st.S.age : 30);
  g.fillStyle = pal["breath-soft"]; g.fillRect(l, t + ch - P.rr[1] / rMax * ch, cw, (P.rr[1] - P.rr[0]) / rMax * ch);
  const bw = cw / B.length;
  B.forEach((x, i) => {
    const h = rates[i] / rMax * ch;
    g.fillStyle = (x.fl & 2) ? pal.warn : pal.breath; g.globalAlpha = 0.8; g.fillRect(l + i * bw + 1, t + ch - h, Math.max(1, bw - 2), h);
    g.globalAlpha = 1; g.fillStyle = pal.gyro; g.beginPath(); g.arc(l + (i + 0.5) * bw, t + ch - amps[i] / aMax * ch, 3, 0, 6.3); g.fill();
    if (x.fl & 4) label(g, l + (i + 0.5) * bw, t + 10, "S", pal.gyro, "center", 10);
  });
  for (let k = 0; k <= 2; k++) { label(g, l - 4, t + ch - k / 2 * ch + 4, (k / 2 * rMax).toFixed(0), pal.breath, "right", 10); label(g, W - r + 4, t + ch - k / 2 * ch + 4, (k / 2 * aMax).toFixed(1) + "°", pal.gyro, "left", 10); }
  label(g, l, H - 3, `last ${B.length} breaths`, pal.muted, "left", 10);
  const per = B.map(x => x.per), ie = B.filter(x => x.ti > 0 && x.te > 0).map(x => x.ti / x.te);
  $("bbbTxt").textContent = `Mean ${meanOf(rates).toFixed(1)} /min (range ${Math.min(...rates).toFixed(0)}-${Math.max(...rates).toFixed(0)}), breath-length variation ${(100 * sdOf(per) / meanOf(per)).toFixed(0)} %, ` +
    `mean depth ${meanOf(amps).toFixed(2)}° (variation ${(100 * sdOf(amps) / meanOf(amps)).toFixed(0)} %)` + (ie.length ? `, I:E 1:${(1 / meanOf(ie)).toFixed(1)}` : "") +
    `. Bars: rate (amber = movement); dots: depth; S = sigh. Shaded band: normal range for the age.`;
}
function drawAdvanced() { if (!anaOpen()) return; drawEnsemble(); drawHist(); drawBreathByBreath(); }
function updateBreathTable() {
  const tb = $("brTable").tBodies[0];
  const rows = st.breaths.filter(b => !(b.fl & 2)).slice(-10).reverse();
  const nowN = st.R.head, now = Date.now();
  tb.innerHTML = rows.map(b => {
    const tt = new Date(now - (nowN - b.n) * 100);
    const ie = b.te ? (b.ti / b.te) : NaN;
    const cls = (b.fl & 1) ? "" : ' style="color:var(--muted)"';
    return `<tr${cls}><td>${tt.toLocaleTimeString([], {hour:"2-digit", minute:"2-digit", second:"2-digit"})}${b.fl & 4 ? " sigh" : ""}${b.fl & 2 ? " moved" : ""}</td><td>${(b.per / 1000).toFixed(1)}</td><td>${(b.ti / 1000).toFixed(1)}</td><td>${(b.te / 1000).toFixed(1)}</td><td>${isNum(ie) && ie > 0 ? "1:" + (1 / ie).toFixed(1) : "--"}</td><td>${b.amp.toFixed(2)}</td></tr>`;
  }).join("") || '<tr><td colspan="6">waiting for breaths</td></tr>';
}

/* ------------------------------ status ------------------------------ */
function setRange(el, state, r, unit) {
  el.className = "range " + (state === "normal" ? "ok" : state || "");
  el.textContent = state ? `${state === "normal" ? "normal" : state === "high" ? "above" : "below"} ${r[0]}-${r[1]} ${unit}` : `normal ${r[0]}-${r[1]} ${unit}`;
}
function sqiBar(bar, txt, q) {
  bar.style.width = (q || 0) + "%";
  bar.style.background = q >= 60 ? pal.ok : q >= 30 ? pal.warn : pal.alert;
  txt.textContent = isNum(q) ? q : "--";
}
/* ------------------------------ v1.3 status hero (plain language) ------------------------------ */
const HERO_ICO = {
  wait: '<path d="M3 12h4l2.5-6 4 12 2.5-6H21"/>',
  ok: '<path d="M20 6.5 9.5 17 4 11.5"/>',
  watch: '<circle cx="12" cy="12" r="9"/><path d="M12 7.5V12l3 2"/>',
  warn: '<path d="M12 3.5 2.5 20h19z"/><path d="M12 10v4.5M12 17.2v.3"/>',
  alert: '<path d="M6 16V11a6 6 0 1 1 12 0v5l2 2H4z"/><path d="M10 20a2 2 0 0 0 4 0"/>',
  ready: '<circle cx="12" cy="12" r="9"/><path d="M10 8.3v7.4l6-3.7z"/>',
  cal: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="2.6"/><path d="M12 1.8v3.4M12 18.8v3.4M1.8 12h3.4M18.8 12h3.4"/>',
  sensor: '<rect x="7" y="7" width="10" height="10" rx="2"/><path d="M4 9.5h3M4 14.5h3M17 9.5h3M17 14.5h3M9.5 4v3M14.5 4v3M9.5 17v3M14.5 17v3"/>',
  move: '<circle cx="13" cy="4.5" r="2"/><path d="M9 21l2.5-6 3 2.5V21M7 11.5l3-3 3.5 1 2.5 3 3 .5M10 8.5 8.5 14"/>'
};
let heroSig = "", heroErr = "", heroErrAt = 0;
function setHero(cls, ico, title, msg, checks, prog, btn) {
  const sig = [cls, ico, title, msg, checks.map(c => c.join(":")).join(";"), prog === null ? "" : Math.round(prog * 100), btn].join("|");
  if (sig === heroSig) return;
  heroSig = sig;
  $("hero").className = "hero s-" + cls;
  $("heroIco").innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true">${HERO_ICO[ico] || HERO_ICO.wait}</svg>`;
  if ($("heroTitle").textContent !== title) $("heroTitle").textContent = title;
  if (Array.isArray(msg)) $("heroMsg").innerHTML = "<ol>" + msg.map(s => `<li>${esc(s)}</li>`).join("") + "</ol>";   // steps
  else $("heroMsg").textContent = msg;
  $("heroChecks").innerHTML = checks.map(([c, t]) => `<li class="${c}">${esc(t)}</li>`).join("");
  $("heroProg").hidden = prog === null;
  if (prog !== null) $("heroProgBar").style.width = Math.round(Math.max(0, Math.min(1, prog)) * 100) + "%";
  const bs = $("btnStart"), bp = $("btnStop");
  bs.hidden = !(btn === "start" || btn === "starting");
  bs.disabled = btn === "starting";
  bs.lastChild.textContent = btn === "starting" ? "Starting..." : "Start measurement";
  bp.hidden = btn !== "stop";
  $("btnSimple").hidden = btn === "none";
  renderSimple();
}
const cap = s => s ? s.charAt(0).toUpperCase() + s.slice(1) : "";
function vitalsLine(S) {
  const p = [];
  const hrOk = S.pres && isNum(S.hr) && !S.art;
  const rrv = isNum(S.rrn) ? S.rrn : isNum(S.rr1) ? S.rr1 : S.rr;
  p.push(hrOk ? `Heart ${Math.round(S.hr)} bpm` : S.art ? "Heart rate paused (strong breathing movement)" : "Heart: looking for a clear beat");
  p.push(isNum(rrv) ? `Breathing ${rrv.toFixed(rrv < 10 ? 1 : 0)} /min` : "Breathing: counting breaths");
  if (isNum(S.b1m) && isNum(rrv)) p.push(`${S.b1m} breaths in the last minute`);
  return p.join(" · ");
}
function renderHero(S, P) {
  const now = Date.now();
  const live = !!S && performance.now() - (st.lastStatusAt || 0) < 6000;
  if (!S || !live) {
    setHero("wait", "wait", st.everStatus ? "Connection to the monitor lost" : "Connecting to the monitor",
      st.everStatus ? "Reconnecting by itself. Stay close to the monitor; if this lasts, check that it is switched on and that this phone is on its Wi-Fi."
        : "Join the monitor's Wi-Fi network (CardioResp-...) if this page does not connect.", [], null, "none");
    $("liveSub").textContent = st.everStatus ? "reconnecting" : "connecting";
    return;
  }
  const errTxt = heroErr && now - heroErrAt < 15000 ? heroErr + " " : "";
  if (S.cal === 3) {
    st.startReq = 0;
    setHero("alert", "sensor", /lost/i.test(S.calmsg || "") ? "Sensor connection lost" : "Sensor not found",
      (S.calmsg || "Check the sensor cable.") + " Check that the 4 sensor wires (3V3, GND, SDA, SCL) are firmly connected. The monitor keeps looking for the sensor by itself.",
      [["no", "Sensor not answering"]], null, "none");
    $("liveSub").textContent = "sensor missing";
    return;
  }
  const pc = placementChecks(S, S.cal === 2);
  if (S.cal === 4) {
    const starting = st.startReq && now - st.startReq < 9000;
    if (st.startReq && !starting) { st.startReq = 0; heroErr = "The monitor did not start - try again."; heroErrAt = now; }
    setHero("ready", "ready", starting ? "Starting the measurement" : "Ready to measure",
      starting ? "Keep still - the monitor is about to measure gravity for 8 seconds." :
        errTxt ? errTxt : ["Fix the sensor on the lower breastbone, Y arrow towards the head (see the drawing below).", "Let the person lie or sit still.",
          "Press Start: 8 seconds of calibration, then measuring begins by itself."],
      [["ok", "Sensor connected"]].concat(pc), null, starting ? "starting" : "start");
    $("liveSub").textContent = "not measuring";
    return;
  }
  st.startReq = 0;
  if (S.cal === 0 || S.cal === 1) {
    const calc = S.mot ? [["bad", "Movement - calibration waits for stillness"]] : [["ok", "Still"]];
    setHero("cal", "cal", S.cal === 0 ? "Starting the measurement" : `Calibrating${S.cala > 1 ? " (attempt " + S.cala + ")" : ""} - keep still`,
      (S.cal === 1 && S.calmsg ? S.calmsg.replace(/^Calibrating - /i, "") + ". " : "") + "The sensor measures gravity for 8 seconds; measuring starts by itself afterwards.",
      calc.concat(pc.filter(c => !/^(Still|Moving)/.test(c[1]))), S.cal === 1 ? (S.calp || 0) : 0.02, "stop");
    $("liveSub").textContent = "calibrating";
    return;
  }
  // ---- measuring
  $("liveSub").textContent = "measuring" + (S.full === 0 ? " (basic mode)" : "");
  const v = st.verd || null;
  const AL = window.CRAlarm && CRAlarm.active ? CRAlarm.active() : [];
  const hrOk = S.pres && isNum(S.hr) && !S.art;
  const rrv = isNum(S.rrn) ? S.rrn : isNum(S.rr1) ? S.rr1 : S.rr;
  const rrOk = isNum(rrv);
  const zH = hrOk && S.sqh >= 40 ? zoneOf(S.hr, P.hr) : 0, zR = rrOk && S.sqr >= 30 ? zoneOf(rrv, P.rr) : 0;
  const line = vitalsLine(S);
  const qc = [];
  qc.push(S.sqh >= 60 ? ["ok", "Heart signal good"] : S.sqh >= 30 ? ["bad", "Heart signal weak"] : ["no", "No clear heart signal"]);
  qc.push(S.sqr >= 60 ? ["ok", "Breathing signal good"] : S.sqr >= 30 ? ["bad", "Breathing signal weak"] : ["no", "No clear breathing signal"]);
  const checks = qc.concat(pc.filter(c => !/^(Lying|Sitting|Propped|Tilted)/.test(c[1])));
  if (S.full === 0) checks.push(["bad", "Basic mode: no licence"]);
  let cls = "ok", ico = "ok", title = "Measuring - readings look normal", msg = line;
  const top = AL.length ? AL[0] : null;
  if (top && top.p >= 3) { cls = "alert"; ico = "alert"; title = top.name; msg = cap(top.t) + ". Check the person now. " + line; }
  else if (top && top.p === 2) { cls = "warn"; ico = "warn"; title = top.name; msg = cap(top.t) + ". " + line; }
  else if (v && v.sev >= 2) { cls = "warn"; ico = "warn"; title = v.head || "Warning"; msg = (v.cond ? v.cond + ". " : "") + line; }
  else if (v && v.sev === 1 && v.code !== "none") { cls = "watch"; ico = "watch"; title = v.head || "Watch"; msg = (v.cond ? v.cond + ". " : "") + line; }
  else if (Math.max(Math.abs(zH), Math.abs(zR)) >= 1) {
    cls = "watch"; ico = "watch";
    title = Math.abs(zH) >= Math.abs(zR) ? (zH > 0 ? "Heart rate above the usual range" : "Heart rate below the usual range") : (zR > 0 ? "Breathing faster than usual" : "Breathing slower than usual");
    msg = line + `. Usual at rest for ${P.name.toLowerCase()}: heart ${P.hr[0]}-${P.hr[1]} bpm, breathing ${P.rr[0]}-${P.rr[1]} /min.`;
  } else if (top && top.p === 1) { cls = "watch"; ico = "watch"; title = top.name; msg = cap(top.t) + ". " + line; }
  else if (S.mot) { cls = "watch"; ico = "move"; title = "Movement - readings pause while the person moves"; msg = "The traces stay visible; rates are updated again a few seconds after the movement stops. " + line; }
  else if (!hrOk && !rrOk) {
    const early = (S.win || 0) < 45;
    cls = early ? "cal" : "watch"; ico = early ? "wait" : "sensor";
    title = early ? "Measuring - first readings in about 30 seconds" : "No clear signal from the body yet";
    msg = early ? "Keep still and breathe normally while the monitor learns the heartbeat and breathing pattern." : "Press the sensor firmly on the lower breastbone and keep still. Check that it did not come loose.";
  } else if (!hrOk || !rrOk) { cls = "ok"; ico = "ok"; title = "Measuring"; msg = line + "."; }
  else msg = line + `. Usual at rest: heart ${P.hr[0]}-${P.hr[1]}, breathing ${P.rr[0]}-${P.rr[1]} /min.`;
  setHero(cls, ico, title, msg, checks, null, "stop");
}
function zoneOf(v, r) {              // same steps as the rule engine: +-1 mild, +-2 moderate, +-3 severe
  if (!isNum(v)) return 0;
  if (v > r[1]) return v <= r[1] * 1.2 ? 1 : v <= r[1] * 1.5 ? 2 : 3;
  if (v < r[0]) return v >= r[0] * 0.83 ? -1 : v >= r[0] * 0.667 ? -2 : -3;
  return 0;
}
async function measureCmd(op) {
  heroErr = "";
  if (op === "start") { st.startReq = Date.now(); st.stopReq = 0; }
  else { st.stopReq = Date.now(); st.startReq = 0; if (window.CRAlarm && CRAlarm.userStop) CRAlarm.userStop(); }
  heroSig = "";
  if (st.S) renderHero(st.S, ageProfile(isNum(st.S.age) ? st.S.age : 30));
  if (send({cmd: op})) return;
  try { await postForm("/api/measure", {op}); }
  catch (e) { st.startReq = 0; heroErr = (op === "start" ? "Could not start: " : "Could not stop: ") + e.message + "."; heroErrAt = Date.now(); heroSig = ""; if (st.S) renderHero(st.S, ageProfile(isNum(st.S.age) ? st.S.age : 30)); }
}
$("btnStart").addEventListener("click", () => measureCmd("start"));
setInterval(() => { if (!st.S || performance.now() - (st.lastStatusAt || 0) > 6000) renderHero(null, null); }, 1000);
$("btnStop").addEventListener("click", () => {
  if (confirm("Stop the measurement? Heart and breathing are no longer watched and the alarms stop until Start is pressed again.")) measureCmd("stop");
});

/* ------------------------------ v1.3 simple full-screen view ------------------------------ */
/* Only the heart rate and the breathing pattern, big enough to read across a room. Alarms keep working on top of it.
   The choice is remembered on this phone; the browser's full-screen mode needs a tap, so it is requested on the button. */
let simpleOn = false, wakeLock = null;
function simpleOpen(user) {
  simpleOn = true; $("simple").hidden = false; document.body.classList.add("simpleon");
  try { localStorage.setItem("cr_simple", "1"); } catch (e) {}
  if (user) {
    const el = document.documentElement, rq = el.requestFullscreen || el.webkitRequestFullscreen;
    if (rq) { try { const pr = rq.call(el); if (pr && pr.catch) pr.catch(() => {}); } catch (e) {} }
  }
  keepAwake(); renderSimple();
}
function simpleClose() {
  simpleOn = false; $("simple").hidden = true; document.body.classList.remove("simpleon");
  try { localStorage.setItem("cr_simple", "0"); } catch (e) {}
  const ex = document.exitFullscreen || document.webkitExitFullscreen;
  if ((document.fullscreenElement || document.webkitFullscreenElement) && ex) { try { const pr = ex.call(document); if (pr && pr.catch) pr.catch(() => {}); } catch (e) {} }
  if (wakeLock) { try { wakeLock.release(); } catch (e) {} wakeLock = null; }
  setTimeout(() => { if (window.CR && CR.redraw) CR.redraw(); }, 60);
}
async function keepAwake() {          // keeps the screen on where the browser allows it (secure pages only)
  if (!simpleOn || wakeLock || !navigator.wakeLock) return;
  try { wakeLock = await navigator.wakeLock.request("screen"); wakeLock.addEventListener("release", () => { wakeLock = null; }); } catch (e) { wakeLock = null; }
}
document.addEventListener("visibilitychange", () => { if (!document.hidden) keepAwake(); });
function renderSimple() {
  if (!simpleOn) return;
  const S = st.S && performance.now() - (st.lastStatusAt || 0) < 6000 ? st.S : null;
  $("sClock").textContent = new Date().toLocaleTimeString([], {hour: "2-digit", minute: "2-digit"});
  const hc = ($("hero").className.match(/s-(\w+)/) || [])[1] || "wait";
  $("sState").className = "sstate s-" + hc;
  $("sState").textContent = $("heroTitle").textContent;
  const P = ageProfile(S && isNum(S.age) ? S.age : 30);
  const meas = !!S && S.cal === 2;
  const hrOk = meas && S.pres && isNum(S.hr) && !S.art;
  const rr = meas ? (isNum(S.rrn) ? S.rrn : isNum(S.rr1) ? S.rr1 : S.rr) : NaN;
  const zc = z => z ? " z" + Math.min(3, Math.abs(z)) : "";
  const hb = $("sHr"), rb = $("sRr");
  hb.innerHTML = (hrOk ? Math.round(S.hr) : "--") + "<small>bpm</small>";
  rb.innerHTML = (isNum(rr) ? rr.toFixed(rr < 10 ? 1 : 0) : "--") + "<small>/min</small>";
  hb.className = "sbig" + (hrOk ? zc(zoneOf(S.hr, P.hr)) : " dim");
  rb.className = "sbig" + (isNum(rr) ? zc(zoneOf(rr, P.rr)) : " dim");
  $("sHrSub").textContent = !meas ? "" : S.art ? "Paused while strong breathing shakes the chest" : `Usual at rest ${P.hr[0]}-${P.hr[1]}`;
  $("sRrSub").textContent = !meas ? "" : (isNum(S.b1m) && isNum(rr) ? `${S.b1m} breaths in the last minute · ` : "") + `usual ${P.rr[0]}-${P.rr[1]}`;
  $("sIdle").hidden = meas;
  if (!meas) {
    $("sIdleMsg").textContent = !S ? "Connecting to the monitor..." : S.cal === 4 ? "Not measuring" : S.cal === 3 ? "Sensor not found - check the cable" : "Calibrating - keep still";
    $("sStart").hidden = !(S && S.cal === 4);
  }
}
function drawSimple() {
  const c = $("cvSimple"), {g, W, H} = fit(c);
  g.clearRect(0, 0, W, H);
  if (W < 40 || H < 40) return;
  const top = 34, bot = 24, y0 = top + (H - top - bot) / 2, hh = (H - top - bot) / 2 * 0.9;
  g.strokeStyle = pal.line; g.lineWidth = 1;
  for (let s2 = 10; s2 < 60; s2 += 10) { const x = Math.round(W - s2 / 60 * W) + 0.5; g.beginPath(); g.moveTo(x, top); g.lineTo(x, H - bot); g.stroke(); label(g, x, H - 7, `-${s2} s`, pal.muted, "center", 11); }
  g.beginPath(); g.moveTo(0, Math.round(y0) + 0.5); g.lineTo(W, Math.round(y0) + 0.5); g.stroke();
  const S = st.S, R = st.R, head = R.head;
  if (!S || S.cal !== 2 || !head) return;
  const N = 600, start = Math.max(0, head - N), X = i => W - (head - 1 - i) / N * W;
  const vals = [];
  for (let i = start; i < head; i++) { const j = i % RN; if (R.have[j] === i) vals.push(R.v[j]); }
  st.scS = smoothScale(st.scS || 0.3, robustMax(vals), 0.08);
  const Y = v => y0 - v / st.scS * hh;
  if (S.apc > 0) { g.fillStyle = pal["heart-soft"]; g.fillRect(X(head - S.apc * 10), top, W, H - top - bot); }
  g.lineWidth = Math.max(2.6, Math.min(4, W / 320)); g.lineJoin = "round"; g.lineCap = "round";
  let pen = false, cur = -1;
  for (let i = start; i < head; i++) {
    const j = i % RN;
    if (R.have[j] !== i) { if (pen) g.stroke(); pen = false; continue; }
    const x = X(i), y = Y(R.v[j]), mv = R.m[j] ? 1 : 0;
    if (pen && mv !== cur) { g.lineTo(x, y); g.stroke(); pen = false; }
    if (!pen) { g.strokeStyle = mv ? pal.warn : (st.zc.r || pal.breath); g.beginPath(); g.moveTo(x, y); pen = true; cur = mv; }
    else g.lineTo(x, y);
  }
  if (pen) g.stroke();
  g.fillStyle = pal.breath;
  for (const b of st.breaths) {
    if (b.pk < start || b.pk >= head || (b.fl & 2)) continue;
    const j = b.pk % RN; if (R.have[j] !== b.pk) continue;
    g.beginPath(); g.arc(X(b.pk), Y(R.v[j]), 4.5, 0, 6.3); g.fill();
  }
}
$("btnSimple").addEventListener("click", () => simpleOpen(true));
$("simpleX").addEventListener("click", simpleClose);
$("sStart").addEventListener("click", () => measureCmd("start"));
document.addEventListener("keydown", e => { if (e.key === "Escape" && simpleOn) simpleClose(); });
setInterval(renderSimple, 1000);

function onStatus(S) {
  const prevCal = st.S ? st.S.cal : -1;
  st.S = S; st.lastStatusAt = performance.now(); st.everStatus = true;
  if (window.CR && CR.onStatus) { try { CR.onStatus(S); } catch (e) { console.error("status hook", e); } }
  const P = ageProfile(isNum(S.age) ? S.age : 30);
  if (prevCal === 2 && S.cal !== 2) resetBuffers();          // v1.3: stopped - do not leave a frozen trace on screen
  // calibration banner (on the Live page the status hero shows calibration, readiness and a missing sensor)
  const cb = $("calBanner");
  const onLive = (window.CR_PAGE || "live") === "live";
  if (S.cal === 1 && !onLive) {
    cb.className = "banner show";
    $("calTitle").textContent = `Calibrating${S.cala > 1 ? " (attempt " + S.cala + ")" : ""} - keep still`;
    $("calMsg").textContent = S.calmsg || "";
    $("calProg").style.width = Math.round((S.calp || 0) * 100) + "%";
  } else if (S.cal === 3 && !onLive) {
    cb.className = "banner show alert";
    $("calTitle").textContent = /lost/i.test(S.calmsg || "") ? "Sensor connection lost" : "Sensor not found";
    $("calMsg").textContent = S.calmsg || "Check the sensor cable.";
    $("calProg").style.width = "0";
  } else if (S.cal === 4 && !onLive) {
    cb.className = "banner show";
    $("calTitle").textContent = "Not measuring";
    $("calMsg").innerHTML = 'The sensor is ready. Open the <a href="#live">Live page</a> and press <b>Start measurement</b>.';
    $("calProg").style.width = "0";
  } else if (S.cal === 2 && /warning/i.test(S.calmsg || "") && calHidden !== S.calmsg) {
    cb.className = "banner show";
    $("calTitle").textContent = "Calibrated with a warning";
    $("calMsg").textContent = S.calmsg.replace(/^Calibrated with a warning:\s*/i, "") + (/movement/i.test(S.calmsg) ? " Press Recalibrate while the person keeps still for the most accurate orientation reference." : "");
    $("calProg").style.width = "100%";
  } else cb.className = "banner";
  $("calProg").parentElement.hidden = S.cal !== 1;
  if (S.net) {
    $("netChip").textContent = S.net.home ? (S.net.host ? S.net.host + ".local" : `Wi-Fi ${S.net.ssid}`) : "Direct connection";
    $("netChip").title = S.net.home ? `Wi-Fi ${S.net.ssid} - http://${S.net.ip}` : "http://192.168.4.1";
  }
  if (S.cal === 2 && S.sid !== undefined) checkSid(S.sid);
  // battery and temperature
  if (S.bat) {
    if (S.bat.ok) { $("battBar").style.width = Math.max(4, S.bat.p) * 0.19 + "px"; $("battBar").style.background = S.bat.p > 20 ? pal.ok : pal.alert; $("battTxt").textContent = `${S.bat.p}% ${f(S.bat.v, 2)} V`; }
    else { $("battBar").style.width = "19px"; $("battBar").style.background = pal.muted; $("battTxt").textContent = "USB power"; }
  }
  if (isNum(S.temp)) {
    const tc = isNum(S.tb) ? S.tb : S.temp, tf = tc * 9 / 5 + 32;
    $("tempChip").textContent = `${isNum(S.tb) && Math.abs(S.tb - S.temp) > 0.05 ? "body" : "sensor"} ${tc.toFixed(1)} °C / ${tf.toFixed(1)} °F`;
  }
  // v1.3: sensor placement and gravity in every state (also before Start and without a licence)
  try { drawOrient(S); } catch (e) { console.error("placement", e); }
  try { renderHero(S, P); } catch (e) { console.error("hero", e); }
  $("btnLog").disabled = S.cal !== 2;
  const heartCard = document.querySelector("article.vital.heart");
  if (heartCard) heartCard.classList.toggle("art", S.cal === 2 && !!S.art);
  if (S.cal !== 2) {
    // not measuring (ready / calibrating / sensor missing): never leave stale vitals on screen
    for (const [id, unit] of [["hrBig", "bpm"], ["rrBig", "/min"]]) { $(id).innerHTML = "--<small>" + unit + "</small>"; $(id).classList.add("dim"); }
    for (const id of ["hrNow", "rmssdTop", "beats60", "fusion", "rrNow", "rr5Top", "b1mTop", "ieTop", "patternTop"]) $(id).textContent = "--";
    setRange($("hrRange"), null, P.hr, "bpm"); setRange($("rrRange"), null, P.rr, "/min");
    sqiBar($("sqiHBar"), $("sqiH"), NaN); sqiBar($("sqiRBar"), $("sqiR"), NaN);
    $("alertBanner").className = "banner alert";
    if (document.activeElement !== $("ageVal") && !ageEdited) showAge(S.age);
    return;
  }
  // heart
  const hrOk = S.pres && isNum(S.hr);
  $("hrBig").innerHTML = (hrOk ? Math.round(S.hr) : "--") + "<small>bpm</small>";
  $("hrBig").classList.toggle("dim", !hrOk);
  $("hrNow").textContent = hrOk && isNum(S.hri) && !S.art ? Math.round(S.hri) : "--";
  $("rmssdTop").textContent = f(S.rmssd, 0);
  setRange($("hrRange"), hrOk && !S.art ? classify(S.hr, P.hr) : null, P.hr, "bpm");
  sqiBar($("sqiHBar"), $("sqiH"), S.sqh);
  $("beats60").textContent = S.pres && isNum(S.v60) ? `${S.v60} of ${S.b60}` : "--";
  $("fusion").textContent = isNum(S.wa) ? `${Math.round(S.wa * 100)}/${Math.round((1 - S.wa) * 100)} %` : "--";
  // breathing (v1.3): big number = newest breaths, then the counted last minute, the 5-minute average and the breath count
  const rrBigV = isNum(S.rrn) ? S.rrn : isNum(S.rr1) ? S.rr1 : S.rr;
  const rrOk = isNum(rrBigV);
  $("rrBig").innerHTML = (rrOk ? rrBigV.toFixed(rrBigV < 10 ? 1 : 0) : "--") + "<small>/min</small>";
  $("rrBig").classList.toggle("dim", !rrOk);
  $("rrNow").textContent = f(S.rr1, 0);
  $("rr5Top").textContent = f(S.rr, 0);
  $("b1mTop").textContent = isNum(S.b1m) && rrOk ? S.b1m : "--";
  $("ieTop").textContent = isNum(S.ie) && S.ie > 0 ? "1:" + (1 / S.ie).toFixed(1) : "--";
  setRange($("rrRange"), rrOk ? classify(rrBigV, P.rr) : null, P.rr, "/min");
  sqiBar($("sqiRBar"), $("sqiR"), S.sqr);
  const pl = patternLabels(S.pf & ~(PF.TACHY | PF.BRADY | PF.IRREG | PF.LOWHRV));
  $("patternTop").textContent = !rrOk ? "--" : pl.length ? pl.join(", ") : (isNum(S.pcv) && S.pcv < 0.2 ? "regular" : "normal");
  // breathing pause banner on the other pages (on the Live page the status hero and the alarm bar show it)
  const ab = $("alertBanner");
  const pause = S.apc >= P.apnea && P.apnea > 0;
  const onLiveP = (window.CR_PAGE || "live") === "live";
  ab.className = "banner alert" + (pause && !onLiveP ? " show" : "");
  $("alertTitle").textContent = pause ? `Breathing pause ${Math.round(S.apc)} s` : "";
  $("alertMsg").textContent = pause ? `No breath detected for ${Math.round(S.apc)} s (alarm limit ${P.apnea} s for this age). Check the person.` : "";
  // system card
  const accOff = S.ab ? Math.hypot(S.ab[0], S.ab[1], S.ab[2]) : NaN;
  $("devSys").innerHTML = kvHtml([
    ["Accelerometer offset corrected", isNum(accOff) ? `${(accOff * 1000).toFixed(0)} mg${accOff > 0.05 ? " (large - see accelerometer calibration)" : ""}` : "--"],
    ["Sample rate", `${f(S.fs, 2)} Hz`], ["Analysis time", `${S.au} ms (max ${S.aumax})`], ["Free memory", `${Math.round(S.heap / 1024)} kB (min ${Math.round(S.minheap / 1024)})`],
    ["FIFO overflows / I2C errors", `${S.ovf} / ${S.i2c}`], ["Lost samples filled", S.gap], ["Phones connected", S.cli],
    ["Gyro offset now", S.bias ? S.bias.map(v => v.toFixed(2)).join(", ") + " °/s" : "--"], ["Uptime", fmtDur(S.up / 1000)]]);
  // trend (1 Hz)
  st.trend.push({t:Date.now(), hr:hrOk && !S.art ? (isNum(S.hri) ? S.hri : S.hr) : NaN, rr:isNum(S.rr1) ? S.rr1 : S.rr});
  if (st.trend.length > 7200) st.trend.splice(0, st.trend.length - 7200);
  drawTrend(); drawTachoPoinc(); updateBreathTable(); drawAdvanced();
  if (Date.now() - st.lastAna > 4000) { st.lastAna = Date.now(); renderAnalysis(); }
  if (document.activeElement !== $("ageVal") && !ageEdited) showAge(S.age);
  updateAgeGroup(S.age);
}
function fmtDur(s) { s = Math.floor(s || 0); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60); return (h ? h + " h " : "") + m + " min " + (s % 60) + " s"; }
function kvHtml(rows) { return rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(String(v))}</dd>`).join(""); }
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({"&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;"}[c])); }
function sectionsHtml(sec) {
  return sec.map(s => `<section><h3>${esc(s.title)}</h3>${s.items.map(([l, t]) => `<div class="finding ${l}"><i></i><span>${esc(t)}</span></div>`).join("")}</section>`).join("");
}
function clientRSA() {
  const beats = st.beats.map(b => ({t:b.i * 10, ibi:b.ibi, valid:!!(b.fl & 1)}));
  const brs = st.breaths.map(b => ({t:b.n * 100, per:b.per, valid:!!(b.fl & 1)}));
  return computeRSA(beats, brs);
}
function renderAnalysis() {
  const S = st.S; if (!S || S.cal !== 2 || S.full === 0) return;
  const P = ageProfile(S.age);
  const sec = liveAnalysis(S, P, {rsa:clientRSA()});
  sec.push({title:"Note", items:[["info", "Automated interpretation for information only. Reference ranges are awake resting values for the entered age; sleep, exercise, fever and medicines change them."]]});
  $("analysis").innerHTML = sectionsHtml(sec);
  $("anaWhen").textContent = "updated " + new Date().toLocaleTimeString([], {hour:"2-digit", minute:"2-digit", second:"2-digit"});
}
function onInfo(m) {
  st.info = m;
  if (window.CR && CR.onInfo) { try { CR.onInfo(m); } catch (e) {} }
  if (m.wsport) wsPort = m.wsport;
  $("devName").textContent = m.dev;
  document.title = "CardioResp " + (m.dev || "").replace(/^CardioResp-/, "");
  const c = m.calib || {}, t = m.st || {};
  const gmTxt = isNum(c.gm) ? `${f(c.gm, 3)} g` + (Math.abs(c.gm - 1) > 0.07 ? " (should be 1.00 - corrected automatically)" : "") : "--";
  $("devSensor").innerHTML = kvHtml([
    ["Status", m.sensor ? "connected" : "not connected"],
    ["Factory self-test", t.ran ? (t.pass ? "passed" : "outside specification") + ` (accel ${t.a.map(v => f(v, 1)).join("/")} %, gyro ${t.g.map(v => f(v, 1)).join("/")} %)` : "not available"],
    ["Start-up calibration", `${c.ok ? "still, OK" : "movement during calibration"} (attempt ${c.att})`],
    ["Gravity reading at start-up", gmTxt],
    ["Gravity reference (x, y, z)", c.g0 ? c.g0.map(v => f(v, 3)).join(", ") : "--"],
    ["Gyro offset at start", c.bias ? c.bias.map(v => f(v, 2)).join(", ") + " °/s" : "--"],
    ["Noise at rest accel / gyro", c.astd ? `${f(Math.max(...c.astd) * 1000, 1)} mg / ${f(Math.max(...c.gstd), 2)} °/s` : "--"],
    ["Offset drift since last start", isNum(c.drift) ? `${f(c.drift, 2)} °/s` : "first start"], ["Firmware", m.fw]]);
  if (!ageEdited) showAge(m.age);
  updateAgeGroup(m.age);
}

/* -------------------------------- age -------------------------------- */
let ageEdited = false;
function showAge(y) {
  if (!isNum(y)) return;
  if (y < 2) { $("ageUnit").value = "m"; $("ageVal").value = Math.round(y * 12); }
  else { $("ageUnit").value = "y"; $("ageVal").value = Math.round(y * 10) / 10; }
}
function updateAgeGroup(y) {
  if (!isNum(y)) return;
  const P = ageProfile(y);
  $("ageGroup").innerHTML = `<b>${esc(P.name)}</b> (${esc(ageText(y))}): heart ${P.hr[0]}-${P.hr[1]} bpm, breathing ${P.rr[0]}-${P.rr[1]} /min, pause alarm at ${P.apnea} s` + (P.rmssd ? `, RMSSD low below ${P.rmssd} ms` : "") + ".";
}
$("ageVal").addEventListener("input", () => { ageEdited = true; });
$("ageApply").addEventListener("click", () => {
  let v = parseFloat($("ageVal").value);
  if (!isNum(v) || v < 0) { $("ageGroup").textContent = "Enter a valid age."; return; }
  if ($("ageUnit").value === "m") v /= 12;
  if (v > 120) { $("ageGroup").textContent = "Enter a valid age."; return; }
  if (send({cmd:"age", v:Math.round(v * 1000) / 1000})) { ageEdited = false; updateAgeGroup(v); $("ageApply").textContent = "Applied"; setTimeout(() => $("ageApply").textContent = "Apply age", 1500); }
  else $("ageGroup").textContent = "Not connected - try again when the status shows Live.";
});

/* ---------------------------- patient data ---------------------------- */
const PT = ["pName", "pId", "pSex", "pPlace", "pPost", "pNotes"];
function patient() { return {name:$("pName").value.trim(), id:$("pId").value.trim(), sex:$("pSex").value, place:$("pPlace").value, post:$("pPost").value, notes:$("pNotes").value.trim()}; }
try { const s = JSON.parse(localStorage.getItem("cr_pt") || "{}"); for (const k of PT) if (s[k] !== undefined) $(k).value = s[k]; } catch (e) {}
for (const k of PT) $(k).addEventListener("change", () => { try { const o = {}; for (const q of PT) o[q] = $(q).value; localStorage.setItem("cr_pt", JSON.stringify(o)); } catch (e) {} });

/* ------------------------------ report ------------------------------ */
function stamp(d) { const p = n => String(n).padStart(2, "0"); return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}`; }
function fileBase() { const p = patient(); const who = (p.id || p.name || "patient").replace(/[^A-Za-z0-9_-]+/g, "_").slice(0, 30); return `CardioResp_${who}_${stamp(st.captured)}`; }
function download(blob, name) {
  const url = URL.createObjectURL(blob), a = document.createElement("a");
  a.href = url; a.download = name; a.rel = "noopener";
  document.body.appendChild(a); a.click();
  setTimeout(() => { URL.revokeObjectURL(url); a.remove(); }, 5000);
}
$("btnLog").addEventListener("click", async () => {
  const b = $("btnLog");
  b.disabled = true; $("logState").textContent = "Downloading the last 10 minutes from the device...";
  try {
    const r = await fetch("/api/log", {cache:"no-store"});
    if (!r.ok) throw new Error(r.status === 503 ? "measurement is not running yet" : "HTTP " + r.status);
    const L = await r.json();
    st.log = L; st.captured = new Date(); st.report = computeReport(L);
    renderReport();
    for (const id of ["btnPdf", "btnCsv", "btnPrint"]) $(id).disabled = false;
    if (navigator.canShare) { $("btnShare").hidden = false; $("btnShare").disabled = false; }
    $("logState").textContent = `Captured ${f(st.report.durMin, 1)} min at ${st.captured.toLocaleTimeString()}. Download the PDF for the doctor or the CSV for further analysis.`;
  } catch (e) {
    $("logState").textContent = "Capture failed: " + e.message + ". Stay connected and try again.";
  }
  b.disabled = false;
});
function renderReport() {
  const R = st.report, L = st.log, P = R.P;
  const wall = t => new Date(st.captured.getTime() - (L.now - t));
  const tf = d => d.toLocaleTimeString([], {hour:"2-digit", minute:"2-digit"});
  $("repWhen").textContent = `${tf(wall(R.t0))}-${tf(wall(R.t1))}, ${f(R.durMin, 1)} min`;
  const rows = [["Heart rate", `${f(R.hrMean, 0)} bpm (${f(R.hrMin, 0)}-${f(R.hrMax, 0)})`, `${P.hr[0]}-${P.hr[1]}`],
    ["RMSSD / SDNN", `${f(R.rmssd, 0)} / ${f(R.sdnn, 0)} ms`, P.rmssd ? `>= ${P.rmssd}` : ""],
    ["Breathing rate", `${f(R.rr, 1)} /min`, `${P.rr[0]}-${P.rr[1]}`], ["I:E", isNum(R.ie) ? "1:" + f(1 / R.ie, 1) : "--", "1:1 - 1:3"],
    [`Pauses >= ${P.apnea} s`, `${R.pauses.length}${R.pauses.length ? " (longest " + f(R.longestPause, 0) + " s)" : ""}`, "0"],
    ["Resp. sinus arrhythmia", `${f(R.rsa, 1)} bpm`, ""], ["Signal quality", `${f(R.sqiH, 0)} / ${f(R.sqiR, 0)}`, ">= 60"]];
  $("reportAnalysis").innerHTML = `<section><div class="tablewrap"><table><thead><tr><th>Measure</th><th>Value</th><th>Reference</th></tr></thead><tbody>${rows.map(r => `<tr><td>${esc(r[0])}</td><td>${esc(r[1])}</td><td>${esc(r[2])}</td></tr>`).join("")}</tbody></table></div></section>` +
    sectionsHtml([{title:"Findings", items:R.findings}]);
  $("reportBox").classList.add("show");
  if (window.CR && CR.onReport) CR.onReport();
}
$("btnPdf").addEventListener("click", () => {
  try {
    const bytes = buildPdf(st.log, st.report, patient(), st.captured);
    download(new Blob([bytes], {type:"application/pdf"}), fileBase() + ".pdf");
  } catch (e) { $("logState").textContent = "PDF failed: " + e.message; }
});
$("btnCsv").addEventListener("click", () => {
  download(new Blob([buildCsv(st.log, st.report, patient(), st.captured)], {type:"text/csv"}), fileBase() + ".csv");
});
$("btnShare").addEventListener("click", async () => {
  try {
    const file = new File([buildPdf(st.log, st.report, patient(), st.captured)], fileBase() + ".pdf", {type:"application/pdf"});
    if (navigator.canShare && navigator.canShare({files:[file]})) await navigator.share({files:[file], title:"CardioResp report"});
    else $("logState").textContent = "Sharing files is not supported here; use Download PDF.";
  } catch (e) {}
});
$("btnPrint").addEventListener("click", () => window.print());

/* ------------------------------ controls ------------------------------ */
$("btnRecal").addEventListener("click", () => {
  if (confirm("Recalibrate now? The person must stay completely still for about 8 seconds.")) send({cmd:"recal"});
});
$("btnReset").addEventListener("click", () => { if (confirm("Restart the measurement? The 10-minute history will be cleared.")) send({cmd:"reset"}); });
$("btnPol").addEventListener("click", () => {
  if (send({cmd:"polarity"})) { const b = $("btnPol"); b.classList.add("on"); setTimeout(() => b.classList.remove("on"), 1200); }
});
$("btnPause").addEventListener("click", e => {
  const b = e.currentTarget; st.frozen = !st.frozen; b.setAttribute("aria-pressed", st.frozen);
  b.querySelector("span").textContent = st.frozen ? "Resume" : "Freeze";
});
/* theme: auto (follows the device), light or dark; remembered on this phone */
const mqDark = window.matchMedia ? matchMedia("(prefers-color-scheme: dark)") : null;
let themeMode = "auto";
function setTheme(night) {
  document.documentElement.setAttribute("data-theme", night ? "night" : "day");
  const m = document.querySelector('meta[name="theme-color"]'); if (m) m.content = night ? "#10161D" : "#FBF6F4";
  readPalette(); gridCache.clear(); drawSpectra(); drawTrend(); drawTachoPoinc();      // repaint only: replaying a status would look like new data
  drawLevel(st.S && Array.isArray(st.S.g) ? st.S : null); if (st.S) drawAxes(st.S);
}
function applyTheme(mode) {
  themeMode = ["auto", "light", "dark"].indexOf(mode) >= 0 ? mode : "auto";
  setTheme(themeMode === "dark" || (themeMode === "auto" && !!(mqDark && mqDark.matches)));
  document.querySelectorAll("#themeSeg button").forEach(b => b.setAttribute("aria-pressed", String(b.dataset.th === themeMode)));
  try { localStorage.setItem("cr_theme", themeMode); } catch (e) {}
}
document.querySelectorAll("#themeSeg button").forEach(b => b.addEventListener("click", () => applyTheme(b.dataset.th)));
if (mqDark) { const h = () => { if (themeMode === "auto") applyTheme("auto"); }; if (mqDark.addEventListener) mqDark.addEventListener("change", h); else if (mqDark.addListener) mqDark.addListener(h); }
st.trendWin = 0;
document.querySelectorAll("#trWin button").forEach(b => b.addEventListener("click", () => {
  st.trendWin = +b.dataset.w; document.querySelectorAll("#trWin button").forEach(x => x.setAttribute("aria-pressed", String(x === b))); drawTrend();
}));
for (const [id, w] of [["w60", 60], ["w300", 300]]) $(id).addEventListener("click", () => { st.win = w; $("w60").setAttribute("aria-pressed", w === 60); $("w300").setAttribute("aria-pressed", w === 300); });

/* ------------------------------- network ------------------------------- */
let netState = null, netTesting = false, wizard = false, calHidden = null;
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function getJson(url, opt) {
  const r = await fetch(url, Object.assign({cache:"no-store"}, opt || {}));
  let j = {};
  try { j = await r.json(); } catch (e) {}
  if (!r.ok) throw new Error(j.error || ("HTTP " + r.status));
  return j;
}
function postForm(url, data) {
  return getJson(url, {method:"POST", headers:{"Content-Type":"application/x-www-form-urlencoded"}, body:new URLSearchParams(data).toString()});
}
function sigWord(r) { return r > -60 ? "strong signal" : r > -70 ? "good signal" : r > -80 ? "weak signal" : "very weak signal"; }
function netMsg(html, bad) { const e = $("netMsg"); e.hidden = !html; e.className = "netmsg" + (bad ? " bad" : ""); e.innerHTML = html || ""; }
async function refreshNet() { if (netTesting) return; try { netState = await getJson("/api/wifi"); renderNet(); } catch (e) {} }
/* v1.3: QR codes (black on white in every theme, so phone cameras read them) */
const qrCache = {};
function qrSvg(text) {
  if (typeof qrcode !== "function" || !text) return "";
  if (qrCache[text]) return qrCache[text];
  try {
    qrcode.stringToBytes = qrcode.stringToBytesFuncs["UTF-8"];
    const q = qrcode(0, "M"); q.addData(text); q.make();
    const n = q.getModuleCount(), m = 3, sz = n + 2 * m;
    let d = "";
    for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) if (q.isDark(r, c)) d += `M${c + m} ${r + m}h1v1h-1z`;
    return (qrCache[text] = `<svg viewBox="0 0 ${sz} ${sz}" role="img" aria-label="QR code: ${esc(text.replace(/;P:[^;]*/, ";P:..."))}" shape-rendering="crispEdges"><rect width="${sz}" height="${sz}" fill="#fff"/><path d="${d}" fill="#000"/></svg>`);
  } catch (e) { return ""; }
}
const wifiQrText = (ssid, pass) => { const q = v => String(v).replace(/([\\;,:"])/g, "\\$1"); return `WIFI:T:${pass ? "WPA" : "nopass"};S:${q(ssid)};${pass ? "P:" + q(pass) + ";" : ""};`; };
function renderNet() {
  const n = netState; if (!n) return;
  $("netChip").textContent = n.home ? (n.host ? n.host + ".local" : `Wi-Fi ${n.saved}`) : "Direct connection";
  const rows = [];
  if (n.home) {
    rows.push(["Connected to", `${n.saved} (${sigWord(n.rssi)})`]);
    rows.push(["Simple address", `http://${n.host}.local` + (n.auto === 0 ? " (name chosen in the set-up)" : " (automatic)")]);
    if (n.uniq && n.uniq !== n.host) rows.push(["Also answers to", `http://${n.uniq}.local`]);
    rows.push(["IP address", `http://${n.ip}` + (n.static ? " (fixed)" : " (from the router, may change)")]);
  } else rows.push(["Mode", "direct connection (the monitor's own network)"]);
  if (n.ap) {
    rows.push(["Own network", n.apSsid + (n.home ? " (on next to the home Wi-Fi)" : "")]);
    rows.push(["Address there", "http://192.168.4.1 (opens by itself after joining)"]);
    if (n.apOff > 0) rows.push(["Own network switches off in", n.apOff + " s"]);
  }
  // share: the simple address and two QR codes (open this monitor / join its own Wi-Fi)
  const openUrl = n.home && n.ip ? `http://${n.ip}/` : "http://192.168.4.1/";
  const shown = n.home && n.host ? `http://${n.host}.local` : "http://192.168.4.1";
  $("netShare").hidden = false;
  $("netAddr").textContent = shown; $("netAddr").href = shown + "/";
  $("netAddr2").textContent = n.home ? `or ${openUrl.replace(/\/$/, "")} on phones that do not open .local names` : `after joining the monitor's own Wi-Fi "${n.apSsid}"`;
  const qo = qrSvg(openUrl); if ($("qrOpen").dataset.t !== openUrl) { $("qrOpen").innerHTML = qo; $("qrOpen").dataset.t = openUrl; }
  $("qrOpenFig").hidden = !qo;
  const jt = n.ap && n.apSsid ? wifiQrText(n.apSsid, n.apPass || "") : "";
  if ($("qrJoin").dataset.t !== jt) { $("qrJoin").innerHTML = jt ? qrSvg(jt) : ""; $("qrJoin").dataset.t = jt; }
  $("qrJoinFig").hidden = !jt;
  $("netApKeepRow").hidden = !n.saved;
  if (document.activeElement !== $("netApKeep")) $("netApKeep").checked = !!n.apKeep;
  if (!n.home && n.saved) rows.push(["Saved home network", n.saved + (n.bootFailed ? " - not reachable, retried while unused" : "")]);
  $("netInfo").innerHTML = kvHtml(rows);
  $("netFix").hidden = !(n.home && !n.static);
  $("netForget").hidden = !n.saved;
  $("netChange").textContent = n.saved ? "Change Wi-Fi network" : "Connect to a Wi-Fi network";
  let skip = false; try { skip = localStorage.getItem("cr_wifi_skip") === "1"; } catch (e) {}
  if (!n.home && !n.saved && !skip && !wizard && $("netForm").hidden) openWizard();
}
function openForm() {
  $("netForm").hidden = false; $("netBtns").hidden = true;
  const n = netState || {};
  $("wHost").value = n.auto === 0 ? (n.host || "") : "";            // empty = automatic simple name
  $("wHost").placeholder = n.auto === 0 ? "automatic" : `automatic (now ${n.host || "cardio"})`;
  if (n.saved && !$("wSsid").value) $("wSsid").value = n.saved;
  if (n.static) { $("wStatic").checked = true; $("wIp").value = n.ip || ""; $("wGw").value = n.gw || ""; $("wMask").value = n.mask || "255.255.255.0"; $("wDns").value = n.dns || ""; }
  $("wSkip").hidden = !wizard; $("wCancel").hidden = wizard;
  scanNets();
}
function closeForm() {
  $("netForm").hidden = true; $("netBtns").hidden = false;
  if (wizard) { wizard = false; $("netHome").appendChild($("netCard")); $("netTitle").textContent = "Network"; }
}
function openWizard() {
  wizard = true;
  $("netTop").appendChild($("netCard"));
  $("netTitle").textContent = "Connect the monitor to your Wi-Fi";
  openForm();
}
async function scanNets() {
  const sel = $("wSel");
  sel.innerHTML = '<option value="">Scanning...</option>';
  let nets = null;
  try {
    await getJson("/api/wifi/scan?start=1");
    for (let k = 0; k < 14 && !nets; k++) {
      await sleep(1200);
      const r = await getJson("/api/wifi/scan");
      if (!r.running) nets = r.nets || [];
    }
  } catch (e) {}
  if (!nets || !nets.length) { sel.innerHTML = '<option value="">No networks found - type the name below</option>'; return; }
  const bars = r => r > -60 ? "strong" : r > -70 ? "good" : r > -80 ? "weak" : "very weak";
  sel.innerHTML = '<option value="">Choose a network</option>' + nets.map(n => `<option value="${esc(n[0])}">${esc(n[0])}  (${bars(n[1])}${n[2] ? ", open" : ""})</option>`).join("");
  const cur = $("wSsid").value;
  if (cur && nets.some(n => n[0] === cur)) sel.value = cur;
}
async function connectNet() {
  const ssid = ($("wSsid").value || $("wSel").value).trim();
  const pass = $("wPass").value;
  const host = $("wHost").value.trim().toLowerCase();
  if (!ssid) { netMsg("Choose a network or type its name.", true); return; }
  if (pass && (pass.length < 8 || pass.length > 63)) { netMsg("Wi-Fi passwords have 8 to 63 characters.", true); return; }
  if (host && !/^[a-z0-9-]{1,31}$/.test(host)) { netMsg("Name of the monitor: use a-z, 0-9 and - only (up to 31 characters), or leave it empty for an automatic name.", true); return; }
  const n = netState || {};
  const data = {ssid, pass, host, static:$("wStatic").checked ? "1" : "0", ip:$("wIp").value.trim(), gw:$("wGw").value.trim(),
    mask:$("wMask").value.trim(), dns:$("wDns").value.trim(), keep:(!pass && ssid === n.saved) ? "1" : "0"};
  $("wConnect").disabled = true;
  netMsg(`Connecting to "${esc(ssid)}" - this takes up to 20 seconds.` + (n.home ? " This page loses its connection if the monitor moves to the other network." :
    " Your phone may briefly drop off the monitor's network; rejoin it if asked."));
  try { await postForm("/api/wifi/connect", data); }
  catch (e) { netMsg(esc(e.message), true); $("wConnect").disabled = false; return; }
  netTesting = true;
  const t0 = Date.now();
  let done = false;
  while (Date.now() - t0 < 60000 && !done) {
    await sleep(1500);
    try {
      const r = await getJson("/api/wifi");
      netState = r;
      if (r.test && r.test.s === 2) { done = true; netTesting = false; netSuccess(r); }
      else if (r.test && r.test.s === 3) { done = true; netTesting = false; netMsg(`Could not connect to "${esc(r.test.ssid)}": ${esc(r.test.msg)}`, true); renderNet(); }
    } catch (e) { /* phone briefly off the monitor's network */ }
  }
  netTesting = false;
  $("wConnect").disabled = false;
  if (!done) netMsg("No answer from the monitor. If your phone left the monitor's network, rejoin it (or your home Wi-Fi) and reload this page.", true);
}
function netSuccess(n) {
  const ip = n.test.ip || n.ip;
  closeForm();
  try { localStorage.removeItem("cr_wifi_skip"); } catch (e) {}
  netMsg(`<b>Connected to "${esc(n.saved)}".</b> The dashboard is now at <a href="http://${ip}/">http://${ip}</a>` +
    (n.auto === 0 ? ` and <a href="http://${esc(n.host)}.local/">http://${esc(n.host)}.local</a>.<br>` : ` and, a few seconds later, at a simple name such as <b>cardio.local</b> (shown on this card).<br>`) +
    `Connect this phone or computer to "${esc(n.saved)}" and open that address (bookmark it).` +
    (n.ap ? ` The monitor's own network switches off in about ${Math.max(0, n.apOff)} s.` : "") +
    (n.static ? "" : `<br><button class="btn" style="margin-top:8px" id="netFix2">Keep ${ip} as a fixed address</button>`));
  const b = $("netFix2"); if (b) b.addEventListener("click", fixAddress);
  renderNet();
}
async function fixAddress() {
  try { const r = await postForm("/api/wifi/static", {}); netMsg(esc(r.msg || "Address fixed.") + ". It stays the same after every restart; if another device ever takes it, reserve it in the router."); refreshNet(); }
  catch (e) { netMsg(esc(e.message), true); }
}
$("netChange").addEventListener("click", () => { netMsg(""); openForm(); });
$("wCancel").addEventListener("click", () => { closeForm(); netMsg(""); });
$("wSkip").addEventListener("click", () => { try { localStorage.setItem("cr_wifi_skip", "1"); } catch (e) {} closeForm(); netMsg(""); });
$("wScan").addEventListener("click", scanNets);
$("wSel").addEventListener("change", () => { if ($("wSel").value) $("wSsid").value = $("wSel").value; });
$("wShow").addEventListener("click", () => { const p = $("wPass"); p.type = p.type === "password" ? "text" : "password"; $("wShow").textContent = p.type === "password" ? "Show" : "Hide"; });
$("wConnect").addEventListener("click", connectNet);
$("netApKeep").addEventListener("change", async e => {
  try { netState = await postForm("/api/wifi/apkeep", {on: e.target.checked ? "1" : "0"}); renderNet(); }
  catch (er) { netMsg(esc(er.message), true); e.target.checked = !e.target.checked; }
});
$("netFix").addEventListener("click", fixAddress);
$("netForget").addEventListener("click", async () => {
  if (!confirm("Forget the home Wi-Fi? The monitor will use only its own network (http://192.168.4.1).")) return;
  try { await postForm("/api/wifi/forget", {}); } catch (e) {}
  netMsg(`Home Wi-Fi forgotten. Join the monitor's own network "${esc((netState && netState.apSsid) || "CardioResp")}" and open http://192.168.4.1`);
  setTimeout(refreshNet, 1500);
});
setInterval(refreshNet, 10000);

/* -------------------- accelerometer 6-position calibration -------------------- */
function onAcal(m) {
  st.acal = m;
  const faces = [["+X up", 1], ["-X up", 2], ["+Y up", 4], ["-Y up", 8], ["+Z up", 16], ["-Z up", 32]];
  $("acalFaces").innerHTML = faces.map(([t, b]) => `<span class="${m.cov & b ? "done" : ""}">${t}</span>`).join("") + `<span>${m.n} captured</span>`;
  $("acalMsg").textContent = m.msg || "";
  $("acalCap").disabled = !!m.cap;
  $("acalCap").textContent = m.cap ? "Capturing - do not touch" : "Capture position";
  $("acalSave").disabled = m.n < 4;
  $("acalSave").textContent = (m.cov === 63) ? "Save calibration" : "Save (offsets only)";
  const c = m.corr || {};
  $("acalRemove").hidden = !c.ok;
  $("acalState").innerHTML = kvHtml(c.ok ? [["Stored calibration", c.full ? "offsets and gains" : "offsets only"], ["Offsets", c.off.map(v => f(v * 1000, 0)).join(", ") + " mg"],
    ["Gains", c.gain.map(v => f(v, 3)).join(", ")], ["Fit error", f(c.rms, 1) + " mg"]] : [["Stored calibration", "none"]]);
}
$("acalCap").addEventListener("click", () => { if (send({cmd:"acal", op:"capture"})) { $("acalCap").disabled = true; $("acalCap").textContent = "Capturing - do not touch"; } });
$("acalSave").addEventListener("click", () => send({cmd:"acal", op:"save"}));
$("acalClear").addEventListener("click", () => send({cmd:"acal", op:"clear"}));
$("acalRemove").addEventListener("click", () => { if (confirm("Remove the stored accelerometer calibration?")) send({cmd:"acal", op:"remove"}); });
onAcal({n:0, cov:0, cap:0, msg:"", corr:{ok:0}});

/* ------------------------------ start-up ------------------------------ */
const ua = navigator.userAgent || "";
if ((/iPhone|iPad/.test(ua) && !/Safari/.test(ua)) || /; wv\)/.test(ua)) $("portalTip").classList.add("show");
let themeSaved = "auto";
try { themeSaved = localStorage.getItem("cr_theme") || (localStorage.getItem("cr_night") === "1" ? "dark" : "auto"); } catch (e) {}
applyTheme(themeSaved);
let lastFrame = 0;
function frame(ts) {
  requestAnimationFrame(frame);
  if (st.frozen || document.hidden || ts - lastFrame < 33) return;
  if (simpleOn) { lastFrame = ts; drawSimple(); return; }        // v1.3: the simple view replaces the page while open
  if (window.CR_PAGE && window.CR_PAGE !== "live") return;      // v1.2: only the visible page is drawn (saves phone battery)
  const pl = $("pg-live");                                       // v1.4: no licence, or traces on a hidden sub-tab: draw nothing
  if (document.body.classList.contains("lite") || pl.classList.contains("sub-place") || pl.classList.contains("sub-more")) return;
  lastFrame = ts;
  drawHeart(); drawResp();
}
requestAnimationFrame(frame);
window.addEventListener("resize", () => { gridCache.clear(); drawSpectra(); drawLevel(st.S && Array.isArray(st.S.g) ? st.S : null); if (st.S) { drawAxes(st.S); drawTrend(); drawTachoPoinc(); } });
drawTrend(); drawTachoPoinc(); updateBreathTable(); drawLevel(null);
window.CR = Object.assign(window.CR || {}, {st, send, $, esc, kvHtml, fmtDur, getJson, postForm, pal: () => pal, setTheme, applyTheme, renderNet, refreshNet, renderAnalysis, download, patient,
  redraw: () => { gridCache.clear(); drawSpectra(); drawLevel(st.S && Array.isArray(st.S.g) ? st.S : null); if (st.S) { drawAxes(st.S); drawTrend(); drawTachoPoinc(); drawAdvanced(); } },
  hrvStats, ensembleData, renderHero, measureCmd, postureOf, simpleOpen, simpleClose, qrSvg, wifiQrText});
renderHero(null, null);
try { if (localStorage.getItem("cr_simple") === "1") simpleOpen(false); } catch (e) {}
connect();
refreshNet();
})();
