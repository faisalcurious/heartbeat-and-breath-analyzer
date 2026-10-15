"use strict";
/* ext.js - CardioResp v1.2 pages: router, assessment (rules + tiny ML), vibration colours, licence, mesh, profiles,
   recording / files (CSV + ZIP), firmware upload, temperature calibration, about.  Uses the bridge window.CR from ui.js. */
(function () {
const CR = window.CR; if (!CR) return;
const $ = id => document.getElementById(id);
const esc = CR.esc, getJson = CR.getJson, postForm = CR.postForm;
const num = v => typeof v === "number" && isFinite(v);
const fx = (v, d) => num(v) ? v.toFixed(d) : "--";
const PAGES = ["live", "people", "mesh", "files", "settings"];
const OLD = {patient: "people", nurse: "people", analysis: "live", about: "settings"};
let page = "live", lastS = null, devVerdict = null, devVerdictAt = 0, meshState = null, about = null, licState = null;
const tracker = typeof AnaTracker === "function" ? new AnaTracker() : null;
const SEVN = ["Normal", "Watch", "Warning", "Alert"];
const zcls = z => "z" + Math.min(3, Math.abs(z));
const REC_KEY = 0x5A17C0DE;          // v1.4: recording file key, must match REC_FILE_KEY in Recorder.h

/* ------------------------------ router ------------------------------ */
function go(p, push) {
  const was = p; if (OLD[p]) p = OLD[p];
  if (PAGES.indexOf(p) < 0) p = "live";
  page = p; window.CR_PAGE = p;
  PAGES.forEach(n => { $("pg-" + n).classList.toggle("show", n === p); });
  document.querySelectorAll(".tabs a").forEach(a => { if (a.dataset.p === p) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current"); });
  if (push !== false && location.hash !== "#" + p) { try { history.replaceState(null, "", "#" + p); } catch (e) { location.hash = p; } }
  window.scrollTo(0, 0);
  if (p === "mesh") { loadMesh(); loadPeers(true); }
  if (p === "files") loadFiles();
  if (p === "settings") { loadLicense(); loadAbout(); loadPower(); loadMeshCfg(); loadAlarmCfg(); }
  if (p === "people") loadProfiles();
  if (p === "live") setTimeout(() => CR.redraw && CR.redraw(), 30);
  if (was === "analysis") { const d = $("anaDrop"); if (d) { showSec(d); d.open = true; setTimeout(() => d.scrollIntoView(), 80); } }
  if (was === "about") setTimeout(() => { const a = $("abName"); if (a) { showSec(a); a.closest(".card").scrollIntoView(); } }, 80);
}
window.addEventListener("hashchange", () => go(location.hash.slice(1), false));

/* ------------------------------ v1.4 sub-tabs ------------------------------ */
/* Long pages show one part at a time. Live: fixed parts (data-sec in index.html, "Graphs only" = just the live traces).
   Settings: one tab per <h2> group. The choice is remembered on this phone. */
const subSel = {};
function subShow(pg, id) {
  subSel[pg.id] = id;
  try { localStorage.setItem("cr_sub_" + pg.id, id); } catch (e) {}
  pg.querySelectorAll("[data-sec]").forEach(e => e.classList.toggle("subhide", e.dataset.sec.split(" ").indexOf(id) < 0));
  pg.querySelectorAll(".subtabs button").forEach(b => b.setAttribute("aria-selected", b.dataset.s === id ? "true" : "false"));
  ["mon", "graphs", "place", "more"].forEach(s => pg.classList.toggle("sub-" + s, s === id));
  if (id === "more") { const d = $("anaDrop"); if (d) d.open = true; }
  setTimeout(() => CR.redraw && CR.redraw(), 30);
}
function showSec(el) {                       // make sure an element is on the visible sub-tab
  const s = el && el.closest("[data-sec]"), pg = el && el.closest(".page");
  if (s && pg && s.classList.contains("subhide")) subShow(pg, s.dataset.sec.split(" ")[0]);
}
(function initSubTabs() {
  const sp = $("pg-settings");
  if (sp && !sp.querySelector(".subtabs")) {
    const short = {"Display and alarms": "Display", "Alarm limits": "Alarm limits", "Calibration": "Calibration", "Mesh network": "Mesh",
      "Power, network and system": "Network", "Restart and reset": "Reset", "Licence and firmware": "Licence", "About": "About"};
    const bar = document.createElement("div"); bar.className = "subtabs"; bar.setAttribute("role", "tablist");
    let k = -1;
    Array.from(sp.children).forEach(e => {
      if (e.tagName === "H2") {
        k++;
        const t = (e.firstChild && e.firstChild.nodeType === 3 ? e.firstChild.textContent : e.textContent).trim();
        const b = document.createElement("button"); b.type = "button"; b.dataset.s = "g" + k; b.textContent = short[t] || t; bar.appendChild(b);
      }
      if (k >= 0) e.dataset.sec = "g" + k;
    });
    if (k > 0) sp.insertBefore(bar, sp.firstChild);
  }
  document.querySelectorAll(".page").forEach(pg => {
    const bar = pg.querySelector(".subtabs"); if (!bar) return;
    const ids = Array.from(bar.querySelectorAll("button")).map(b => b.dataset.s);
    let cur = null; try { cur = localStorage.getItem("cr_sub_" + pg.id); } catch (e) {}
    bar.addEventListener("click", ev => { const b = ev.target.closest("button"); if (b && b.dataset.s) subShow(pg, b.dataset.s); });
    subShow(pg, ids.indexOf(cur) >= 0 ? cur : ids[0]);
  });
})();
CR.showSec = showSec;

/* ------------------------------ time sync ------------------------------ */
function syncTime() {
  const e = Date.now() / 1000;
  if (!CR.send({cmd: "time", v: e})) postForm("/api/time", {v: String(e)}).catch(() => {});
}

/* ------------------------------ zones and colours ------------------------------ */
function zone(v, r) {                     // same zone steps as the rule engine (Analytics.h)
  if (!num(v)) return 0;
  const [lo, hi] = r;
  if (v > hi) return v <= hi * 1.2 ? 1 : v <= hi * 1.5 ? 2 : 3;
  if (v < lo) return v >= lo * 0.83 ? 1 : v >= lo * 0.667 ? 2 : 3;
  return 0;
}
function setZone(el, z, on) {
  if (!el) return;
  el.classList.remove("z0", "z1", "z2", "z3");
  if (on) { el.classList.add(zcls(z)); el.classList.remove("dim"); }
}
function vibZones(S) {
  return {
    a: !num(S.vra) ? 0 : S.vra > 120 ? 3 : S.vra > 60 ? 2 : S.vra > 30 ? 1 : 0,
    pk: !num(S.vpk) ? 0 : S.vpk > 400 ? 3 : S.vpk > 200 ? 2 : S.vpk > 100 ? 1 : 0,
    g: !num(S.vrg) ? 0 : S.vrg > 6 ? 3 : S.vrg > 3 ? 2 : S.vrg > 1.5 ? 1 : 0,
    hf: !num(S.vhf) ? 0 : S.vhf >= 0.6 ? 3 : S.vhf >= 0.45 ? 2 : S.vhf >= 0.35 ? 1 : 0,
    cg: !num(S.cgh) ? 0 : S.cgh >= 8 ? 3 : S.cgh >= 4 ? 2 : S.cgh >= 2 ? 1 : 0
  };
}
function tempZone(c) { return !num(c) ? 0 : c >= 39.5 ? 3 : c >= 38.0 ? 2 : c >= 37.6 ? 1 : c < 34.5 ? 3 : c < 35.5 ? 1 : 0; }

/* ------------------------------ status hook ------------------------------ */
function onStatus(S) {
  lastS = S;
  const full = S.full !== 0;
  const P = ageProfile(num(S.age) ? S.age : 30);
  const measuring = S.cal === 2;
  const hrZ = zone(S.pres === 0 || S.art ? NaN : S.hr, P.hr);
  const rrDisp = num(S.rrn) ? S.rrn : num(S.rr1) ? S.rr1 : S.rr;       // v1.3: the big number shows the newest breaths
  const rrZ = zone(rrDisp, P.rr);
  const tb = num(S.tbody) ? S.tbody : S.tb;
  const tz = tempZone(tb);
  // assessment: the monitor's own verdict while it is fresh, otherwise the in-page rules + ML
  let verd = null, pv = null, fresh = false;
  if (full && measuring) {
    try { pv = pageVerdict(S, P); } catch (e) { pv = null; }
    fresh = !!devVerdict && performance.now() - devVerdictAt < (CR.st.connected ? 20000 : 6000);
    verd = fresh ? devVerdict : pv;
  }
  CR.st.verd = verd;                                                // read by the status hero (ui.js)
  // alarms first, so a display problem can never stop them
  if (S.alv !== undefined && S.alv !== almSeen) { almSeen = S.alv; loadAlarmCfg(); }      // v1.3: limits changed on the monitor
  const lim = effLimits(P);
  try { if (window.CRAlarm) { if (CRAlarm.setLimits) CRAlarm.setLimits(lim); CRAlarm.update(S, {P, hrZ, zone, tz, tb, vz: full && measuring ? vibZones(S) : null, v: verd, full, lim}); } } catch (e) { console.error("alarms", e); }
  try {
    document.body.classList.toggle("lite", !full);
    ["spectraSec", "spectraSecH", "analysis"].forEach(i => { const e = $(i); if (e) e.classList.add("fullonly"); });   // v1.3: placement is never hidden
    licBanner(S);
    const hb = $("hrBig"), rb = $("rrBig");
    setZone(hb, hrZ, measuring && num(S.hr) && S.pres !== 0 && !S.art); setZone(rb, rrZ, measuring && num(rrDisp));
    setZone($("hrNow"), zone(S.hri, P.hr), measuring && num(S.hri) && !S.art);
    if (full) {
      const v = vibZones(S);
      const set = (id, val, d, z) => { const e = $(id); e.textContent = fx(val, d); setZone(e, z, num(val)); };
      set("vibA", S.vra, 1, v.a); set("vibPk", S.vpk, 0, v.pk); set("vibG", S.vrg, 2, v.g);
      set("vibHf", num(S.vhf) ? S.vhf * 100 : NaN, 0, v.hf); set("vibCg", S.cgh, 0, v.cg);
    }
    $("tbC").textContent = fx(tb, 1); $("tbF").textContent = num(tb) ? fx(tb * 9 / 5 + 32, 1) : "--";
    setZone($("tbC"), tz, num(tb)); setZone($("tbF"), tz, num(tb));
    if (S.hot) $("tbHint").textContent = "The chip is hot: the monitor is lowering its stream rate to cool down.";
    if (S.rec) recStatus(S.rec);
    if (!measuring) showIdle(S);
    else if (full) showVerdict(verd, fresh ? "monitor" : "this page", pv, fresh ? devVerdict : null);
    else showLite(S, P);
    { const pb = $("btnPol"); if (pb && S.pol !== undefined) pb.title = "Breathing direction " + (S.pol < 0 ? "inverted" : "normal") + " - tap to flip"; }
    if (S.mesh !== undefined) $("meshTog").textContent = S.mesh ? "Switch mesh off" : "Switch mesh on";
    perf(S);
  } catch (e) { console.error("status display", e); }
}
let trSid = null, trLast = 0;
function pageVerdict(S, P) {
  if (!tracker || typeof anaEvaluate !== "function" || typeof ML_MODEL === "undefined" || (!num(S.hr) && !num(S.rr))) return null;
  const s = anaInputs(Object.assign({}, S, {tbody: num(S.tbody) ? S.tbody : S.tb}));
  const now = (S.up || S.now || Date.now());
  if (S.sid !== trSid || now < trLast) { tracker.reset(); trSid = S.sid; }       // new measurement session or monitor restarted
  trLast = now;
  tracker.update(s, P, now);
  return anaEvaluate(tracker, s, P, now, ML_MODEL);
}
function clearMeters() {
  for (const id of ["sevScore", "sevRel", "sevSrc", "sevMl", "sevNews"]) { const e = $(id); if (e) e.textContent = "--"; }
  $("sevEv").innerHTML = ""; $("sevMiss").innerHTML = ""; $("sevBars").innerHTML = "";
  const o = $("obsRow"); if (o) o.hidden = true;
}
function showIdle(S) {
  const c = $("sevCard"); c.className = "card assess s0";
  $("sevBadge").className = "sev"; $("sevBadge").textContent = S.cal === 3 ? "No sensor" : S.cal === 4 ? "Not measuring" : "Starting";
  $("sevHead").textContent = S.cal === 3 ? "The sensor is not connected" : S.cal === 4 ? "Press Start measurement to begin" : "Calibrating the sensor";
  $("sevCond").textContent = S.cal === 4 ? "The assessment starts with the measurement. Long-term checks (infection, sleep apnoea, lasting irregular rhythm) need 5 to 30 calm minutes of measuring." : "";
  clearMeters();
}
function rangeWord(v, r, unit, d) {
  if (!num(v)) return "--";
  const t = `${v.toFixed(d || 0)} ${unit}`;
  return v > r[1] ? `${t} (above the usual ${r[0]}-${r[1]})` : v < r[0] ? `${t} (below the usual ${r[0]}-${r[1]})` : `${t} (usual ${r[0]}-${r[1]})`;
}
function showLite(S, P) {
  const c = $("sevCard");
  const hz = S.pres && num(S.hr) && !S.art ? zone(S.hr, P.hr) : 0;
  const rv = num(S.rrn) ? S.rrn : num(S.rr1) ? S.rr1 : S.rr, rz = zone(rv, P.rr);
  const out = Math.max(hz, rz);
  c.className = "card assess s" + Math.min(2, out);
  $("sevBadge").className = "sev" + (out ? " s" + Math.min(2, out) : " s0"); $("sevBadge").textContent = out ? "Outside range" : "Basic";
  $("sevHead").textContent = out ? (hz >= rz ? "Heart rate outside the usual range" : "Breathing rate outside the usual range") : "Heart rate, breathing and placement";
  $("sevCond").textContent = `Heart ${S.art ? "paused (strong breathing movement)" : rangeWord(S.pres ? S.hr : NaN, P.hr, "bpm")} · Breathing ${rangeWord(rv, P.rr, "/min", rv < 10 ? 1 : 0)}. ` +
    "Basic mode (no valid licence): the full assessment, heart-signal-lost and cough alarms, waveforms and spectra are off. Rate and breathing-pause alarms keep working.";
  clearMeters();
}
function showVerdict(v, src, pv, dv) {
  const c = $("sevCard");
  if (!v) { c.className = "card assess s0"; $("sevBadge").className = "sev"; $("sevBadge").textContent = "Waiting"; $("sevHead").textContent = "Waiting for measurements"; $("sevCond").textContent = ""; clearMeters(); return; }
  const sev = Math.max(0, Math.min(3, v.sev | 0));
  c.className = "card assess s" + sev;
  const b = $("sevBadge"); b.className = "sev s" + sev; b.textContent = SEVN[sev];
  $("sevHead").textContent = v.head || "No abnormality found";
  $("sevCond").textContent = v.cond || "";
  // v1.3: illness patterns are only named after enough calm monitoring - show how far that has come
  const o = $("obsRow");
  if (o) {
    const need = num(v.need) && v.need > 0 ? v.need : 30, obs = num(v.obs) ? v.obs : 0;
    o.hidden = false; o.classList.toggle("done", obs >= need);
    $("obsTxt").textContent = obs >= need ? `Long-term checks active (${obs} calm minutes in the last hour)` : `Long-term checks: ${obs} of ${need} calm minutes`;
    $("obsBar").style.width = Math.min(100, Math.round(100 * obs / need)) + "%";
  }
  { const n = $("sevNews"); if (n) n.textContent = num(v.news) && v.news >= 0 ? `${v.news} (${v.news >= 7 ? "high" : v.news >= 5 ? "medium" : "low"} risk)` : "after 5 min (adults)"; }
  $("sevScore").textContent = fx(v.score, 2); $("sevRel").textContent = (v.rel | 0) + " / 100"; $("sevSrc").textContent = src;
  let ml = v.mln || "--";
  if (dv && pv && pv.mln !== dv.mln) ml += ` (page: ${pv.mln})`;
  $("sevMl").textContent = ml;
  $("sevEv").innerHTML = (v.ev || []).map(e => `<li class="${esc(e[0])}"><i></i><span>${esc(e[1])}</span></li>`).join("");
  $("sevMiss").innerHTML = (v.miss || []).map(m => `<li>${esc(m)}</li>`).join("");
  const names = typeof ANA_CLASS !== "undefined" ? ANA_CLASS : [];
  $("sevBars").innerHTML = (v.p || []).map((p, i) => `<span>${esc(names[i] || i)}</span><span class="m"><i style="width:${Math.round(p * 100)}%"></i></span><span>${Math.round(p * 100)} %</span>`).join("");
}
function perf(S) {
  const e = $("perfInfo"); if (!e || page !== "settings") return;
  powerInfo(S);
  e.innerHTML = CR.kvHtml([["Processor clock", num(S.mhz) ? S.mhz + " MHz" + (S.boost ? " (temporary boost)" : "") : "--"], ["Chip temperature", num(S.chip) ? S.chip + " °C" + (S.hot ? " (throttling)" : "") : "--"],
    ["Analysis time", num(S.au) ? `${S.au} ms (max ${S.aumax} ms)` : "--"], ["Free memory", num(S.heap) ? Math.round(S.heap / 1024) + " kB" : "--"],
    ["Browsers connected", S.cli], ["FIFO overflows", S.ovf], ["I2C errors", S.i2c], ["Mesh nodes heard", S.mn]]);
}

/* ------------------------------ licence ------------------------------ */
function licBanner(S) {
  const L = S.lic || {}; const b = $("licBanner");
  if (!L.en) { b.className = "lic"; return; }
  if (!L.full) {
    b.className = "lic show";
    $("licTitle").textContent = L.exp ? "Your licence has expired" : L.tok ? "The activation key is not valid for this monitor" : "No licence activated";
    $("licMsg").textContent = "Only heart rate and breathing rate are shown. " + (L.exp ? "Request a new licence and enter the new key in Settings." : "Enter an activation key in Settings.");
    waLink();
  } else if (L.eat && L.clk && L.eat - L.now < 7 * 86400) {
    b.className = "lic show soft";
    $("licTitle").textContent = (L.eat - L.now < 86400 ? "Licence ends in less than a day" : "Licence ends in " + Math.ceil((L.eat - L.now) / 86400) + " day(s)");
    $("licMsg").textContent = "Request a new licence before it expires.";
    waLink();
  } else b.className = "lic";
}
let waBusy = false;
function waLink() {
  const a = $("licWa"), mac = lastS && lastS.lic ? lastS.lic.mac : "";
  const wa = about && about.wa ? about.wa.replace(/\D/g, "") : "";
  if (wa) { a.href = `https://wa.me/${wa}?text=${encodeURIComponent("Please renew the licence for CardioResp monitor " + (mac || ""))}`; a.target = "_blank"; }
  else if (!about && !waBusy) { waBusy = true; loadAbout().then(() => { setTimeout(() => { waBusy = false; }, 15000); if (about) waLink(); }); }
}
async function loadLicense() {
  try { licState = await getJson("/api/license"); renderLicense(); } catch (e) {}
}
function fmtDate(t) { return t ? new Date(t * 1000).toLocaleString() : "--"; }
function renderLicense() {
  const L = licState || (lastS && lastS.lic) || {};
  const state = !L.en ? "not required" : L.full ? "active" : L.exp ? "expired" : L.tok ? (L.macok === 0 ? "wrong device" : "invalid") : "not activated";
  $("licInfo").innerHTML = CR.kvHtml([["Status", state], ["Device ID (MAC)", L.mac || "--"], ["Licensed to", L.user || "--"], ["Issued", fmtDate(L.iat)],
    ["Expires", L.eat ? fmtDate(L.eat) : "--"], ["Clock", L.clk ? "known" : "not set yet"], ["Decryption key", L.key ? "present" : "missing in firmware"]]);
}
$("licAct").onclick = async () => {
  const tok = $("licTok").value.trim(); if (!tok) { $("licMsg2").textContent = "Paste the activation key first."; return; }
  syncTime();
  try { const r = await postForm("/api/license", {token: tok}); licState = r; renderLicense(); $("licMsg2").textContent = r.msg || "Activated."; $("licTok").value = ""; }
  catch (e) { $("licMsg2").textContent = e.message; loadLicense(); }
};
$("licDel").onclick = async () => { if (!confirm("Remove the activation key from this monitor?")) return; try { licState = await postForm("/api/license/remove", {}); renderLicense(); $("licMsg2").textContent = "Key removed."; } catch (e) { $("licMsg2").textContent = e.message; } };
$("licCopy").onclick = () => { const m = (licState || (lastS && lastS.lic) || {}).mac || ""; if (navigator.clipboard) navigator.clipboard.writeText(m).then(() => { $("licMsg2").textContent = "Copied " + m; }, () => { $("licMsg2").textContent = m; }); else $("licMsg2").textContent = m; };
$("licGo").onclick = () => { go("settings"); showSec($("licTok")); setTimeout(() => $("licTok").focus(), 100); };

/* ------------------------------ about ------------------------------ */
async function loadAbout() {
  if (!about) { try { about = await getJson("/api/about"); } catch (e) { return about; } }
  const A = about, wa = (A.wa || "").replace(/\D/g, "");
  $("abName").textContent = A.name; $("abOrg").textContent = A.org;
  $("abMail").textContent = A.email; $("abMail").href = "mailto:" + A.email;
  $("abTel").textContent = A.phone; $("abTel").href = "tel:" + A.phone.replace(/[^+\d]/g, "");
  const wl = wa ? "https://wa.me/" + wa : "#";
  $("abWa").textContent = wa ? "+" + wa : "--"; $("abWa").href = wl; $("abWaBtn").href = wl;
  $("abInfo").innerHTML = CR.kvHtml([["Firmware", A.fw], ["Built", A.build], ["ML model", A.ml], ["Device", A.dev], ["MAC", A.mac], ["CPU", A.cpu + " MHz"],
    ["Memory", Math.round(A.flash / 1048576) + " MB"], ["Program size", Math.round(A.sketch / 1024) + " kB"], ["Update", A.ota === "password" ? "password protected" : "open"]]);
  { const fv = $("fwVer"); if (fv) fv.textContent = A.fw; } $("fwKeyRow").hidden = A.ota !== "password";
  return A;
}

/* ------------------------------ profiles ------------------------------ */
const PF_P = [["Name", "name"], ["Id", "id"], ["Sex", "sex"], ["Bed", "bed"], ["Place", "place"], ["Post", "post"], ["Notes", "notes"]];
const PF_N = [["Name", "name"], ["Id", "id"], ["Ward", "ward"], ["Phone", "phone"], ["Shift", "shift"]];
let profLoaded = false;
async function loadProfiles() {
  try {
    const j = await getJson("/api/profile");
    PF_P.forEach(([s, k]) => { const e = $("dp" + s); if (e && document.activeElement !== e) e.value = j.patient[k] || ""; });
    PF_N.forEach(([s, k]) => { const e = $("nu" + s); if (e && document.activeElement !== e) e.value = j.nurse[k] || ""; });
    $("nuPat").innerHTML = CR.kvHtml([["Patient", j.patient.name || "--"], ["ID", j.patient.id || "--"], ["Bed", j.patient.bed || "--"], ["Notes", j.patient.notes || "--"]]);
    profLoaded = true;
  } catch (e) {}
}
async function saveProf(kind, fields, pre, msg) {
  const d = {kind}; fields.forEach(([s, k]) => { d[k] = $(pre + s).value; });
  try { await postForm("/api/profile", d); $(msg).textContent = "Saved."; loadProfiles(); } catch (e) { $(msg).textContent = e.message; }
}
$("dpSave").onclick = () => saveProf("patient", PF_P, "dp", "dpMsg");
$("nuSave").onclick = () => saveProf("nurse", PF_N, "nu", "nuMsg");

/* ------------------------------ mesh ------------------------------ */
async function loadMesh() { try { renderMesh(await getJson("/api/mesh")); } catch (e) { $("meshInfo").textContent = e.message; } }
function renderMesh(m) {
  meshState = m;
  $("meshTog").textContent = m.on ? "Switch mesh off" : "Switch mesh on";
  $("meshAll").disabled = !m.on;
  $("meshSub").textContent = m.on ? (m.up ? `${(m.nodes || []).length} monitor(s)` + (m.grp ? ` - group "${m.grp}"` + (m.open ? " (no PIN)" : "") : "") : "radio not ready") : "switched off";
  const unv = (m.near || []).length;
  $("meshInfo").innerHTML = m.on && unv ? `<b>${unv} monitor(s) nearby are not verified</b> (other group, wrong PIN or older software). See Settings &rarr; Mesh network &rarr; Verify nearby monitors.` :
    "Every verified monitor (same group name and PIN, same Wi-Fi channel) is listed here with its patient, heart rate, breathing rate and severity. \"Update all\" asks every monitor for its newest data at once.";
  if (page !== "mesh") return;
  const nodes = (m.nodes || []).slice().sort((a, b) => (b.self | 0) - (a.self | 0) || (b.sev | 0) - (a.sev | 0));
  if (!m.on) { $("meshGrid").innerHTML = '<div class="card">Mesh is switched off. Switch it on here and on the other monitors (same Wi-Fi channel).</div>'; return; }
  $("meshGrid").innerHTML = nodes.map(n => {
    const sev = Math.min(3, n.sev | 0), old = !n.self && n.ago > 30;
    const lite = !(n.fl & 4);
    return `<div class="node s${sev}${old ? " old" : ""}"><h4><span>${esc(n.name || n.id)}${n.lbl ? ` <small class="small">${esc(n.lbl)}</small>` : ""}${n.self ? " (this monitor)" : ""}</span><span class="sev s${sev}">${lite ? "Basic" : SEVN[sev]}</span></h4>
      <div class="nv"><div><b class="${(n.fl & 1) && num(n.hr) ? "" : "dim"}">${fx(n.hr, 0)}</b><small>heart /min</small></div><div><b>${fx(n.rr, 0)}</b><small>breaths /min</small></div>
      <div><b>${num(n.t) ? fx(n.t, 1) : "--"}</b><small>&deg;C${num(n.t) ? " / " + fx(n.t * 9 / 5 + 32, 1) + " &deg;F" : ""}</small></div></div>
      <div class="small">${lite ? "" : "ML: " + esc((typeof ANA_CLASS !== "undefined" ? ANA_CLASS[n.mlc] : n.mlc) || "") + " " + fx(n.mls, 2) + " &middot; reliability " + (n.rel | 0) + " &middot; "}signal ${n.sqh | 0}/${n.sqr | 0}${n.fl & 2 ? " &middot; moving" : ""}</div>
      <div class="small">ID ${esc(n.id)} &middot; ${n.self ? "local" : (n.ago | 0) + " s ago &middot; " + (n.rssi | 0) + " dBm &middot; " + (n.hops | 0) + " hop(s)"}</div></div>`;
  }).join("") || '<div class="card">No monitors heard yet.</div>';
}
$("meshTog").onclick = async () => { try { renderMesh(await postForm("/api/mesh", {op: meshState && meshState.on ? "off" : "on"})); } catch (e) { $("meshInfo").textContent = e.message; } };
$("meshAll").onclick = async () => { try { $("meshAll").disabled = true; renderMesh(await postForm("/api/mesh", {op: "refresh"})); setTimeout(loadMesh, 1500); setTimeout(loadMesh, 4000); } catch (e) { $("meshInfo").textContent = e.message; } setTimeout(() => { $("meshAll").disabled = !(meshState && meshState.on); }, 1500); };

/* ------------------------------ v1.3 monitors on the same Wi-Fi network ------------------------------ */
let peerBusy = false;
async function loadPeers(scan) {
  if (peerBusy) return;
  peerBusy = true; $("peerScan").disabled = true;
  if (scan) $("peerLead").textContent = "Looking for monitors on this Wi-Fi...";
  try {
    let j = await getJson("/api/peers" + (scan ? "?scan=1" : ""));
    for (let k = 0; k < 10 && j.running; k++) { await new Promise(r => setTimeout(r, 600)); j = await getJson("/api/peers"); }
    renderPeers(j);
  } catch (e) { $("peerLead").textContent = "Could not search: " + (e.message || e); }
  peerBusy = false; $("peerScan").disabled = false;
}
function renderPeers(j) {
  const peers = j.peers || [];
  const card = (name, sub, url, ipUrl, me) => `<div class="peer${me ? " me" : ""}"><a href="${esc(url)}"${me ? "" : ' target="_blank" rel="noopener"'}><b>${esc(name)}</b></a><span>${esc(sub)}</span>` +
    (ipUrl ? `<span><a href="${esc(ipUrl)}" target="_blank" rel="noopener">${esc(ipUrl.replace(/^http:\/\/|\/$/g, ""))}</a></span>` : "") + "</div>";
  let html = card((j.me || "this monitor") + ".local", "this monitor", "/", "", true);
  html += peers.map(p => card(p.host ? p.host + ".local" : p.ip, p.lbl ? p.lbl : "no bed / room label", p.host ? `http://${p.host}.local/` : `http://${p.ip}/`, p.ip ? `http://${p.ip}/` : "", false)).join("");
  $("peerList").innerHTML = html;
  $("peerLead").innerHTML = !j.home ? "This monitor is not on a Wi-Fi network (it uses its own network), so other monitors cannot be listed. Connect it to the Wi-Fi in <b>Settings &rarr; Network</b>." :
    peers.length ? `${peers.length} other monitor${peers.length > 1 ? "s" : ""} on this Wi-Fi. Tap a name to open it (or the number, on phones that do not open .local names). The bed / room label is set under Settings &rarr; Mesh network.` :
    "No other monitor found on this Wi-Fi yet. Monitors switched on later are found with <b>Search again</b>.";
}
$("peerScan").onclick = () => loadPeers(true);

/* ------------------------------ alarm and event log ------------------------------ */
const LVN = {3: "urgent", 2: "warning", 1: "notice"};
const p2e = v => String(v).padStart(2, "0");
const hms = t => { const d = new Date(t); return `${p2e(d.getHours())}:${p2e(d.getMinutes())}:${p2e(d.getSeconds())}`; };
const durTxt = ms => { const s = Math.round(ms / 1000); return s < 60 ? s + " s" : Math.floor(s / 60) + " min " + (s % 60) + " s"; };
let evSig = "";
function renderEvents() {
  if (!window.CRAlarm || !$("evTab")) return;
  const L = CRAlarm.events().slice().reverse(), now = Date.now();
  const sig = L.length + ":" + L.map(e => e.t1 + e.p).join(",") + (L.some(e => !e.t1) ? Math.floor(now / 5000) : "");
  if (sig === evSig) return; evSig = sig;
  $("evTab").tBodies[0].innerHTML = L.map(e => `<tr><td>${hms(e.t0)}</td><td><b>${esc(e.name)}</b></td><td><span class="vst ${e.p === 3 ? "pin" : e.p === 2 ? "grp" : "old"}">${LVN[e.p] || ""}</span></td>` +
    `<td>${esc(e.t)}</td><td>${e.t1 ? durTxt(e.t1 - e.t0) : "active " + durTxt(now - e.t0)}</td></tr>`).join("") || '<tr><td colspan="5">No alarms since this page was opened.</td></tr>';
}
setInterval(() => { const d = $("anaDrop"); if (!d || d.open) renderEvents(); }, 1000);
$("evCsv").onclick = () => {
  const L = CRAlarm.events(), q = v => /[",\n]/.test(v) ? '"' + String(v).replace(/"/g, '""') + '"' : String(v);
  const iso = t => { const d = new Date(t); return `${d.getFullYear()}-${p2e(d.getMonth() + 1)}-${p2e(d.getDate())} ${hms(t)}`; };
  const rows = ["start,end,duration_s,alarm,level,details"].concat(L.map(e => [iso(e.t0), e.t1 ? iso(e.t1) : "active", Math.round(((e.t1 || Date.now()) - e.t0) / 1000), q(e.name), LVN[e.p] || "", q(e.t)].join(",")));
  save(`CardioResp_events_${iso(Date.now()).replace(/[: ]/g, "-")}.csv`, new Blob([rows.join("\n") + "\n"], {type: "text/csv"}));
};

/* ------------------------------ mesh settings and verification ------------------------------ */
let mgCfg = null;
const VST = {0: ["ok", "verified"], 1: ["grp", "other group"], 2: ["pin", "wrong PIN"], 3: ["old", "older software"]};
async function loadMeshCfg() { try { mgCfg = await getJson("/api/mesh/config"); renderMeshCfg(); } catch (e) { $("mgMsg").textContent = e.message; } }
function renderMeshCfg() {
  const c = mgCfg; if (!c) return;
  const idle = id => document.activeElement !== $(id);
  if (idle("mgGrp")) $("mgGrp").value = c.grp || "";
  if (idle("mgLbl")) $("mgLbl").value = c.lbl || "";
  $("mgPer").value = String(c.per); $("mgTtl").value = String(c.ttl); $("mgOn").checked = !!c.on;
  $("mgOpen").checked = !c.pinSet; $("mgPin").disabled = !c.pinSet && $("mgOpen").checked;
  $("mgPin").placeholder = c.pinSet ? `${"\u2022".repeat(c.pinLen || 4)} (unchanged)` : "no PIN set";
  $("mgInfo").innerHTML = CR.kvHtml([["Mesh", c.on ? "on" : "off"], ["Group", c.grp], ["PIN", c.pinSet ? `set (${c.pinLen} digits)` : "none - open mesh"],
    ["This monitor", c.id + (c.lbl ? " - " + c.lbl : "")], ["Radio channel", c.ch || "--"]]);
}
$("mgOpen").onchange = () => { $("mgPin").disabled = $("mgOpen").checked; if ($("mgOpen").checked) $("mgPin").value = ""; };
$("mgPinShow").onclick = () => { const i = $("mgPin"); i.type = i.type === "password" ? "text" : "password"; $("mgPinShow").textContent = i.type === "password" ? "Show" : "Hide"; };
$("mgSave").onclick = async () => {
  const grp = $("mgGrp").value.trim(), pin = $("mgPin").value.trim(), lbl = $("mgLbl").value.trim(), open = $("mgOpen").checked;
  if (!/^[A-Za-z0-9 _-]{1,15}$/.test(grp)) { $("mgMsg").textContent = "Group name: 1 to 15 letters, digits, spaces, - or _."; return; }
  if (!open && pin && !/^[0-9]{4,8}$/.test(pin)) { $("mgMsg").textContent = "PIN: 4 to 8 digits."; return; }
  if (!open && !pin && !(mgCfg && mgCfg.pinSet)) { $("mgMsg").textContent = "Enter a PIN, or tick \"No PIN\"."; return; }
  if (lbl.length > 15) { $("mgMsg").textContent = "Label: at most 15 characters."; return; }
  const body = {grp, lbl, per: $("mgPer").value, ttl: $("mgTtl").value, on: $("mgOn").checked ? "1" : "0"};
  if (open) body.pinclear = "1"; else if (pin) body.pin = pin;
  try {
    mgCfg = await postForm("/api/mesh/config", body); $("mgPin").value = ""; renderMeshCfg();
    $("mgMsg").textContent = "Saved. Use the same group name and PIN on every monitor that should share data, then press Verify.";
    loadMesh();
  } catch (e) { $("mgMsg").textContent = "Not saved: " + (e.message || e); }
};
function renderVerify(m) {
  const rows = [];
  for (const n of (m.nodes || [])) if (!n.self) rows.push({n, st: 0});
  for (const n of (m.near || [])) rows.push({n, st: n.st | 0});
  rows.sort((a, b) => a.st - b.st || (b.n.rssi | 0) - (a.n.rssi | 0));
  $("mgTab").tBodies[0].innerHTML = rows.map(({n, st}) => {
    const v = VST[st] || VST[2], who = [n.lbl, n.name].filter(Boolean).map(esc).join(" &middot; ") || esc(n.id);
    return `<tr><td><b>${who}</b><small>ID ${esc(n.id)}</small></td><td>${n.rssi | 0} dBm<small>${(n.ago | 0)} s ago${n.hops ? " &middot; " + n.hops + " hop(s)" : ""}</small></td><td><span class="vst ${v[0]}">${v[1]}</span></td></tr>`;
  }).join("") || '<tr><td colspan="3">No other monitor answered. Check that the other monitors are switched on, have the mesh on and are within a few metres.</td></tr>';
  const ok = rows.filter(r => r.st === 0).length, bad = rows.length - ok;
  $("mgVMsg").textContent = `${ok} verified, ${bad} not verified` + (bad ? ". Monitors with \"wrong PIN\" or \"other group\" need the same group name and PIN as this one." : ".");
}
$("mgVerify").onclick = async () => {
  if (mgCfg && !mgCfg.on) { $("mgVMsg").textContent = "Switch the mesh on and save first."; return; }
  $("mgVerify").disabled = true; $("mgVMsg").textContent = "Asking nearby monitors...";
  try {
    await postForm("/api/mesh", {op: "verify"});
    await new Promise(r => setTimeout(r, 3500));
    renderVerify(await getJson("/api/mesh"));
  } catch (e) { $("mgVMsg").textContent = e.message; }
  $("mgVerify").disabled = false;
};

/* ------------------------------ temperature calibration ------------------------------ */
$("tcSave").onclick = async () => {
  try { const r = await postForm("/api/temp", {ref: $("tcRef").value, unit: $("tcUnit").value}); $("tcMsg").textContent = "Offset saved: " + fx(r.offset, 2) + " °C (" + fx(r.offset * 9 / 5, 2) + " °F)"; }
  catch (e) { $("tcMsg").textContent = e.message; }
};
$("tcReset").onclick = async () => { try { await postForm("/api/temp", {op: "reset"}); $("tcMsg").textContent = "Offset cleared."; } catch (e) { $("tcMsg").textContent = e.message; } };

/* ------------------------------ firmware update ------------------------------ */
$("fwFile").onchange = () => { const f = $("fwFile").files[0]; $("fwGo").disabled = !f; if (f) $("fwMsg").textContent = `${f.name} (${Math.round(f.size / 1024)} kB)`; };
$("fwGo").onclick = () => {
  const f = $("fwFile").files[0]; if (!f) return;
  if (!/\.bin$/i.test(f.name)) { $("fwMsg").textContent = "Choose a .bin file."; return; }
  if (!confirm("Install " + f.name + "? The monitor will restart.")) return;
  const fd = new FormData(); fd.append("firmware", f, f.name);
  const x = new XMLHttpRequest(); const key = $("fwKey").value;
  x.open("POST", "/api/update" + (key ? "?key=" + encodeURIComponent(key) : ""));
  x.upload.onprogress = e => { if (e.lengthComputable) $("fwProg").style.width = Math.round(e.loaded / e.total * 100) + "%"; };
  x.onload = () => {
    let j = {}; try { j = JSON.parse(x.responseText); } catch (e) {}
    if (x.status === 200 && j.ok) { $("fwProg").style.width = "100%"; $("fwMsg").textContent = "Installed. The monitor is restarting; this page reconnects by itself."; setTimeout(() => location.reload(), 12000); }
    else $("fwMsg").textContent = "Update failed: " + (j.error || ("HTTP " + x.status));
    $("fwGo").disabled = false;
  };
  x.onerror = () => { $("fwMsg").textContent = "Connection lost during the upload. If the monitor restarted, the update may have worked."; $("fwGo").disabled = false; };
  $("fwGo").disabled = true; $("fwMsg").textContent = "Uploading..."; x.send(fd);
};

/* ------------------------------ recording and files ------------------------------ */
function recStatus(r) {
  $("recGo").disabled = !!r.on; $("recStop").hidden = !r.on;
  $("recProg").style.width = r.on && r.t ? Math.round(r.s / r.t * 100) + "%" : "0";
  if (r.on) $("recMsg").textContent = `Recording ${r.n}: ${r.s} / ${r.t} s`;
  if (recWas && !r.on) { recWas = false; $("recMsg").textContent = "Recording saved."; if (page === "files") loadFiles(); }
  recWas = !!r.on;
}
let recWas = false;
$("recGo").onclick = async () => { try { const r = await postForm("/api/rec", {op: "start", sec: $("recSec").value}); $("recMsg").textContent = "Recording to " + r.name; } catch (e) { $("recMsg").textContent = e.message; } };
$("recStop").onclick = async () => { try { await postForm("/api/rec", {op: "stop"}); } catch (e) {} setTimeout(loadFiles, 500); };
let fileList = [];
async function loadFiles() {
  try {
    const j = await getJson("/api/files"); fileList = j.files || [];
    $("fileFree").textContent = j.ok ? `${Math.round(j.free / 1024)} kB free of ${Math.round(j.total / 1024)} kB` : "flash storage unavailable";
    $("fileTab").tBodies[0].innerHTML = fileList.map(f => `<tr><td>${esc(f.n)}</td><td>${Math.round(f.s / 102.4) / 10} kB</td><td><a href="#" class="pdfl" data-pdf="${esc(f.n)}">PDF</a> &middot; <a href="#" data-csv="${esc(f.n)}">CSV</a> &middot; <a href="/api/file?n=${encodeURIComponent(f.n)}">raw</a> &middot; <a href="#" data-del="${esc(f.n)}">delete</a></td></tr>`).join("") || '<tr><td colspan="3">No recordings yet</td></tr>';
    if (j.rec) recStatus({on: 1, s: j.sec, t: j.tot, n: j.cur});
  } catch (e) { $("fileMsg").textContent = e.message; }
}
$("fileTab").onclick = async e => {
  const a = e.target.closest("a"); if (!a) return;
  if (a.dataset.pdf) { e.preventDefault(); recPdf([a.dataset.pdf]); return; }
  if (a.dataset.csv) { e.preventDefault(); try { const c = await toCsv(a.dataset.csv); save(c.name, c.blob); } catch (x) { $("fileMsg").textContent = x.message; } }
  else if (a.dataset.del) { e.preventDefault(); if (!confirm("Delete " + a.dataset.del + "?")) return; try { await postForm("/api/file/delete", {n: a.dataset.del}); loadFiles(); } catch (x) { $("fileMsg").textContent = x.message; } }
};
$("fileDelAll").onclick = async () => { if (!confirm("Delete all recordings from the monitor?")) return; try { await postForm("/api/file/delete", {all: "1"}); loadFiles(); } catch (x) { $("fileMsg").textContent = x.message; } };
$("fileRaw").onclick = async () => { for (const f of fileList) { const a = document.createElement("a"); a.href = "/api/file?n=" + encodeURIComponent(f.n); a.download = f.n; document.body.appendChild(a); a.click(); a.remove(); await new Promise(r => setTimeout(r, 400)); } };
$("fileAll").onclick = async () => {
  if (!fileList.length) { $("fileMsg").textContent = "Nothing to download."; return; }
  try {
    $("fileMsg").textContent = "Collecting..."; const entries = [];
    for (const f of fileList) { const c = await toCsv(f.n); entries.push({name: c.name, data: c.bytes}); }
    save("cardioresp_recordings.zip", new Blob([zip(entries)], {type: "application/zip"})); $("fileMsg").textContent = `${entries.length} file(s) downloaded.`;
  } catch (x) { $("fileMsg").textContent = x.message; }
};
function save(name, blob) { const u = URL.createObjectURL(blob), a = document.createElement("a"); a.href = u; a.download = name; document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(u), 4000); }
function cstr(dv, o, n) {
  const b = []; for (let i = 0; i < n; i++) { const c = dv.getUint8(o + i); if (!c) break; b.push(c); }
  try { return new TextDecoder("utf-8").decode(new Uint8Array(b)); } catch (e) { return String.fromCharCode.apply(null, b); }
}
function csvq(s) { return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s; }
async function parseCrr(name) {
  const r = await fetch("/api/file?n=" + encodeURIComponent(name), {cache: "no-store"});
  if (!r.ok) throw new Error("Cannot read " + name);
  const buf = await r.arrayBuffer(), dv = new DataView(buf); let o = 0, H = null; const rows = [], D = [], R = [];
  while (o + 3 <= buf.byteLength) {
    const tag = String.fromCharCode(dv.getUint8(o)), len = dv.getUint16(o + 1, true); o += 3;
    if (o + len > buf.byteLength) break;
    if (tag === "H" && len >= 68) {
      H = {epoch: dv.getUint32(o, true), up: dv.getUint32(o + 4, true), sec: dv.getUint16(o + 8, true), age: dv.getFloat32(o + 10, true),
        dev: cstr(dv, o + 14, 12), name: cstr(dv, o + 26, 24), pid: cstr(dv, o + 50, 16), full: dv.getUint8(o + 66)};
      if (dv.getUint8(o + 67) >= 2) {           // v1.4: rest of the file is encrypted - decrypt in place
        let s = ((REC_KEY ^ H.epoch ^ Math.imul(H.up, 2654435761)) | 1) >>> 0;
        const u8 = new Uint8Array(buf);
        for (let i = o + len; i < u8.length; i++) { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; u8[i] ^= s & 255; }
      }
    }
    else if (tag === "V" && len >= 12) rows.push({hr: dv.getUint16(o, true) / 10, rr: dv.getUint16(o + 2, true) / 10, sqh: dv.getUint8(o + 4), sqr: dv.getUint8(o + 5), sev: dv.getUint8(o + 6), fl: dv.getUint8(o + 7), tc: dv.getInt16(o + 8, true) / 100, rm: dv.getUint16(o + 10, true) / 10});
    else if (tag === "D") for (let i = 0; i + 4 <= len; i += 4) D.push([dv.getInt16(o + i, true), dv.getInt16(o + i + 2, true)]);
    else if (tag === "R") for (let i = 0; i + 2 <= len; i += 2) R.push(dv.getInt16(o + i, true));
    else if (tag === "d" || tag === "r") {      // v1.4: zig-zag varint deltas, restarting at 0 in each chunk
      let i = o, p0 = 0, p1 = 0; const end = o + len;
      const rd = () => { let z = 0, sh = 0, b; do { b = dv.getUint8(i++); z += (b & 127) * 2 ** sh; sh += 7; } while (b & 128 && i < end); return (z % 2) ? -(z + 1) / 2 : z / 2; };
      if (tag === "r") while (i < end) { p0 += rd(); R.push(p0); }
      else while (i < end) { p0 += rd(); if (i >= end) break; p1 += rd(); D.push([p0, p1]); }
    }
    o += len;
  }
  H = H || {epoch: 0, age: NaN, dev: "", name: "", pid: "", full: 0};
  return {name, H, rows, D, R};
}
async function toCsv(name) {
  const {H, rows, D, R} = await parseCrr(name);
  const L = [`# CardioResp recording ${name}`, `# device,${csvq(H.dev)},patient,${csvq(H.name)},id,${csvq(H.pid)},age_years,${fx(H.age, 2)},start,${H.epoch ? new Date(H.epoch * 1000).toISOString() : "clock unknown"}`,
    "second,hr_bpm,rr_per_min,signal_heart,signal_resp,severity,heart_present,motion,temp_c,temp_f,rmssd_ms"];
  rows.forEach((x, i) => L.push([i + 1, x.hr || "", x.rr || "", x.sqh, x.sqr, x.sev, x.fl & 1, (x.fl >> 1) & 1, x.tc > -100 ? x.tc.toFixed(2) : "", x.tc > -100 ? (x.tc * 9 / 5 + 32).toFixed(2) : "", x.rm || ""].join(",")));
  if (D.length) { L.push("", "# vibration waveform at 100 Hz (raw units): sample,scg,gcg"); D.forEach((d, i) => L.push(i + "," + d[0] + "," + d[1])); }
  if (R.length) { L.push("", "# chest tilt at 10 Hz (millidegrees): sample,tilt_mdeg"); R.forEach((d, i) => L.push(i + "," + d)); }
  const bytes = new TextEncoder().encode(L.join("\n") + "\n");
  return {name: name.replace(/\.crr$/, "") + ".csv", bytes, blob: new Blob([bytes], {type: "text/csv"})};
}
/* ------------------------------ PDF results report from stored recordings ------------------------------ */
const SEVC = ["#2F7D4F", "#9A5F0C", "#C2570C", "#B42332"], SEVNONE = "#C9C3C1";
const p2 = v => String(v).padStart(2, "0");
const isoT = d => `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`;
const isoD = d => `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} ${isoT(d)}`;
function statsOf(vals) {
  const v = vals.filter(num); if (!v.length) return {n: 0, mean: NaN, min: NaN, max: NaN};
  let s = 0, mn = Infinity, mx = -Infinity; for (const x of v) { s += x; if (x < mn) mn = x; if (x > mx) mx = x; }
  return {n: v.length, mean: s / v.length, min: mn, max: mx};
}
function outside(vals, r) { const v = vals.filter(num); if (!v.length || !r) return NaN; return 100 * v.filter(x => x < r[0] || x > r[1]).length / v.length; }
function recSeries(rec) {
  const rows = rec.rows;
  return {
    hr: rows.map(x => (x.fl & 1) && x.hr > 0 ? x.hr : NaN),
    rr: rows.map(x => x.rr > 0 ? x.rr : NaN),
    tc: rows.map(x => x.tc > -100 && x.tc < 100 ? x.tc : NaN),
    rm: rows.map(x => x.rm > 0 ? x.rm : NaN),
    sqh: rows.map(x => x.sqh), sqr: rows.map(x => x.sqr),
    sev: rows.map(x => (x.fl & 4) ? Math.min(3, x.sev | 0) : -1),      // -1 = no assessment in that second (no licence / not ready)
    mot: rows.map(x => (x.fl >> 1) & 1)
  };
}
function recFindings(rec, X, P) {
  const F = [], hr = statsOf(X.hr), rr = statsOf(X.rr), tc = statsOf(X.tc), n = rec.rows.length;
  const pct = v => Math.round(v) + " %";
  if (!hr.n) F.push(["warn", "No reliable heart rate in this recording (check the sensor contact and placement)."]);
  else {
    const o = outside(X.hr, P.hr);
    if (hr.mean > P.hr[1]) F.push(["alert", `Average heart rate ${hr.mean.toFixed(0)} bpm is above the reference ${P.hr[0]}-${P.hr[1]} bpm (suspected tachycardia).`]);
    else if (hr.mean < P.hr[0]) F.push(["alert", `Average heart rate ${hr.mean.toFixed(0)} bpm is below the reference ${P.hr[0]}-${P.hr[1]} bpm (suspected bradycardia).`]);
    else F.push(["ok", `Average heart rate ${hr.mean.toFixed(0)} bpm is within the reference ${P.hr[0]}-${P.hr[1]} bpm.`]);
    if (o >= 10) F.push(["warn", `Heart rate was outside the reference range for ${pct(o)} of the valid seconds (range ${hr.min.toFixed(0)}-${hr.max.toFixed(0)} bpm).`]);
  }
  if (!rr.n) F.push(["warn", "No reliable breathing rate in this recording (a breathing rate needs about 30 s of still breathing)."]);
  else {
    const o = outside(X.rr, P.rr);
    if (rr.mean > P.rr[1]) F.push(["alert", `Average breathing rate ${rr.mean.toFixed(1)} /min is above the reference ${P.rr[0]}-${P.rr[1]} /min (suspected tachypnoea).`]);
    else if (rr.mean < P.rr[0]) F.push(["alert", `Average breathing rate ${rr.mean.toFixed(1)} /min is below the reference ${P.rr[0]}-${P.rr[1]} /min (suspected bradypnoea).`]);
    else F.push(["ok", `Average breathing rate ${rr.mean.toFixed(1)} /min is within the reference ${P.rr[0]}-${P.rr[1]} /min.`]);
    if (o >= 10) F.push(["warn", `Breathing rate was outside the reference range for ${pct(o)} of the valid seconds.`]);
  }
  if (tc.n) {
    if (tc.max >= 38.0) F.push(["alert", `Temperature reached ${tc.max.toFixed(1)} °C (${(tc.max * 9 / 5 + 32).toFixed(1)} °F): possible fever. Valid only if the temperature was calibrated on the skin.`]);
    else if (tc.min < 35.5) F.push(["warn", `Temperature fell to ${tc.min.toFixed(1)} °C (${(tc.min * 9 / 5 + 32).toFixed(1)} °F). The sensor may not have been warmed to the skin, or the temperature is not calibrated.`]);
    else F.push(["ok", `Temperature ${tc.min.toFixed(1)}-${tc.max.toFixed(1)} °C (${(tc.min * 9 / 5 + 32).toFixed(1)}-${(tc.max * 9 / 5 + 32).toFixed(1)} °F).`]);
  }
  const sevMax = X.sev.reduce((a, b) => Math.max(a, b), -1), nA = X.sev.filter(v => v >= 0).length;
  if (n && !nA) F.push(["warn", "No assessment was recorded (no valid licence, or the assessment was not ready yet)."]);
  else if (sevMax >= 2) { const s2 = X.sev.filter(v => v >= 2).length, s3 = X.sev.filter(v => v === 3).length; F.push(["alert", `The assessment was Warning or Alert for ${s2} s of ${n} s` + (s3 ? ` (Alert ${s3} s).` : ".")]); }
  else if (sevMax === 1) F.push(["warn", `The assessment showed Watch for ${X.sev.filter(v => v === 1).length} s; no Warning or Alert.`]);
  else if (n) F.push(["ok", `The assessment stayed Normal${nA < n ? ` (assessed for ${nA} of ${n} s)` : " for the whole recording"}.`]);
  const mot = n ? 100 * X.mot.reduce((a, b) => a + b, 0) / n : 0;
  if (mot >= 30) F.push(["warn", `Movement during ${pct(mot)} of the recording: rates may be less reliable.`]);
  const sq = statsOf(X.sqh);
  if (sq.n && sq.mean < 50) F.push(["warn", `Average heart signal quality ${sq.mean.toFixed(0)} / 100 is low: fix the sensor more firmly or move it to the lower breastbone.`]);
  return F;
}
function recPdfBytes(recs, prof) {
  const pdf = new Pdf(), M = 40, W = pdf.W - 2 * M, now = new Date();
  const pp = (prof && prof.patient) || {}, nu = (prof && prof.nurse) || {};
  const dfmt = isoD;
  const sect = (y, t) => { pdf.text(M, y, t, {size: 11, bold: true}); pdf.line(M, y + 4, M + W, y + 4, 0.6, "#B42332"); return y + 18; };
  const kvBlock = (x, y, title, rows, w) => {
    pdf.text(x, y, title, {size: 9, bold: true, color: "#1D5FA8"}); y += 13;
    for (const [k, v] of rows) { pdf.text(x, y, k, {size: 8.2, color: "#5E6676"}); pdf.text(x + 70, y, String(v || "--").slice(0, Math.floor((w - 72) / 4.2)), {size: 8.2}); y += 11.5; }
    return y;
  };
  const chart = (x, y, w, h, vals, band, col, title, unit, dec, fRight) => {      // fRight: °F scale on the right (temperature)
    const v = vals.filter(num);
    pdf.text(x, y - 4, title, {size: 8.5, bold: true, color: col});
    pdf.rect(x, y, w, h, {fill: "#FFFFFF", stroke: "#E3D8D4", w: 0.6});
    if (!v.length) { pdf.text(x + w / 2, y + h / 2 + 3, "no valid values", {size: 8, color: "#5E6676", align: "center"}); return; }
    let lo = Math.min(...v), hi = Math.max(...v);
    if (band) { lo = Math.min(lo, band[0]); hi = Math.max(hi, band[1]); }
    const pad = Math.max((hi - lo) * 0.12, dec ? 0.2 : 2); lo -= pad; hi += pad;
    const Y = q => y + h - (q - lo) / (hi - lo) * h, n = vals.length;
    if (band) pdf.rect(x, Y(band[1]), w, Y(band[0]) - Y(band[1]), {fill: "#E9F4EC"});
    for (let i = 0; i <= 4; i++) {
      const q = lo + (hi - lo) * i / 4, yy = Y(q);
      pdf.line(x, yy, x + w, yy, 0.25, "#EBDDD8");
      pdf.text(x - 3, yy + 2.5, q.toFixed(dec || 0), {size: 6.5, color: "#5E6676", align: "right"});
      if (fRight) pdf.text(x + w + 3, yy + 2.5, (q * 9 / 5 + 32).toFixed(1), {size: 6.5, color: "#5E6676"});
    }
    const pts = vals.map((q, i) => num(q) ? [x + (n > 1 ? i / (n - 1) : 0.5) * w, Y(q)] : null);
    pdf.poly(pts, col, 1.1);
    for (let s = 0; s <= n - 1; s += Math.max(10, Math.ceil(n / 60) * 10)) pdf.text(x + (n > 1 ? s / (n - 1) : 0) * w, y + h + 9, s + " s", {size: 6.5, color: "#5E6676", align: "center"});
    pdf.text(x + w, y - 4, unit, {size: 7, color: "#5E6676", align: "right"});
  };
  for (const rec of recs) {
    const H = rec.H, X = recSeries(rec), ageRec = num(H.age) && H.age > 0, age = ageRec ? H.age : (lastS && num(lastS.age) ? lastS.age : 30), P = ageProfile(age);
    const n = rec.rows.length, start = H.epoch ? new Date(H.epoch * 1000) : null;
    /* ---------- page 1 ---------- */
    pdf.page();
    pdf.rect(0, 0, pdf.W, 72, {fill: "#FBF1EF"});
    pdf.text(M, 32, "Recording results report", {size: 18, bold: true});
    pdf.text(M, 50, `${rec.name} - ${n} s${H.full ? " with waveforms" : ""} - ${H.dev || "CardioResp"}`, {size: 9, color: "#5E6676"});
    pdf.text(M + W, 32, start ? dfmt(start) : "clock unknown", {size: 9, bold: true, align: "right"});
    pdf.text(M + W, 50, P.name, {size: 9, color: "#5E6676", align: "right"});
    let y = 92;
    const cw = (W - 20) / 3;
    const NS = "not stored";
    const y1 = kvBlock(M, y, "Patient", [["Name", H.name || NS], ["Patient ID", H.pid || NS], [ageRec ? "Age" : "Age *", ageText(age)], ["Sex *", pp.sex], ["Bed / room *", pp.bed]], cw);
    const y2 = kvBlock(M + cw + 10, y, "Nurse *", [["Name", nu.name], ["Staff ID", nu.id], ["Ward", nu.ward], ["Phone", nu.phone], ["Shift", nu.shift]], cw);
    const y3 = kvBlock(M + 2 * (cw + 10), y, "Recording", [["Start", start ? isoT(start) : "clock unknown"], ["Length", n + " s"], ["Placement *", pp.place], ["Posture *", pp.post], ["Waveforms", H.full ? "included" : "not included"]], cw);
    pdf.text(M, Math.max(y1, y2, y3) + 2, "* from the profile stored on the monitor now (it may have changed since the recording was made)", {size: 7, color: "#5E6676"});
    y = Math.max(y1, y2, y3) + 14;
    y = sect(y, "Summary");
    const hr = statsOf(X.hr), rr = statsOf(X.rr), tc = statsOf(X.tc), rm = statsOf(X.rm), sqh = statsOf(X.sqh), sqr = statsOf(X.sqr);
    const motPct = n ? 100 * X.mot.reduce((a, b) => a + b, 0) / n : NaN;
    const f1 = (v, d) => num(v) ? v.toFixed(d) : "--", op = (vals, r) => { const o = outside(vals, r); return num(o) ? Math.round(o) + " %" : "--"; };
    const T = [["Measure", "Mean", "Min", "Max", "Reference", "Outside range"],
      ["Heart rate (bpm)", f1(hr.mean, 0), f1(hr.min, 0), f1(hr.max, 0), `${P.hr[0]}-${P.hr[1]}`, op(X.hr, P.hr)],
      ["Breathing rate (/min)", f1(rr.mean, 1), f1(rr.min, 1), f1(rr.max, 1), `${P.rr[0]}-${P.rr[1]}`, op(X.rr, P.rr)],
      ["RMSSD (ms)", f1(rm.mean, 0), f1(rm.min, 0), f1(rm.max, 0), P.rmssd ? `>= ${P.rmssd}` : "", ""],
      ["Temperature (°C)", f1(tc.mean, 1), f1(tc.min, 1), f1(tc.max, 1), "35.5-37.5", op(X.tc, [35.5, 37.5])],
      ["Temperature (°F)", num(tc.mean) ? (tc.mean * 9 / 5 + 32).toFixed(1) : "--", num(tc.min) ? (tc.min * 9 / 5 + 32).toFixed(1) : "--", num(tc.max) ? (tc.max * 9 / 5 + 32).toFixed(1) : "--", "95.9-99.5", ""],
      ["Signal quality heart / breathing", `${f1(sqh.mean, 0)} / ${f1(sqr.mean, 0)}`, "", "", ">= 60", ""],
      ["Movement (% of time)", f1(motPct, 0), "", "", "< 30", ""]];
    const cx = [M, M + 190, M + 245, M + 295, M + 345, M + W];
    T.forEach((r, i) => {
      if (i === 0) pdf.rect(M, y - 9, W, 13, {fill: "#F6EEEC"});
      r.forEach((c, j) => pdf.text(j === 0 ? cx[0] + 3 : j === 5 ? cx[5] - 3 : cx[j] + 40, y, c, {size: 8.4, bold: i === 0, align: j === 0 ? "left" : "right"}));
      pdf.line(M, y + 4, M + W, y + 4, 0.25, "#E3D8D4"); y += 14;
    });
    y += 8;
    y = sect(y, "Assessment over time");
    const cnt = [0, 0, 0, 0, 0]; X.sev.forEach(v => cnt[v < 0 ? 4 : v]++);
    const bw = W / Math.max(1, n);
    X.sev.forEach((v, i) => pdf.rect(M + i * bw, y, bw + 0.3, 12, {fill: v < 0 ? SEVNONE : SEVC[v]}));
    pdf.rect(M, y, W, 12, {stroke: "#C9B7B2", w: 0.5});
    y += 24;
    let lx = M;
    SEVN.concat(["No assessment"]).forEach((nm, i) => { if (i === 4 && !cnt[4]) return; pdf.rect(lx, y - 7, 8, 8, {fill: i === 4 ? SEVNONE : SEVC[i]}); pdf.text(lx + 12, y, `${nm} ${cnt[i]} s`, {size: 8.4}); lx += i === 3 ? 90 : 95; });
    y += 16;
    y = sect(y, "Findings");
    for (const [lvl, t] of recFindings(rec, X, P)) {
      pdf.dot(M + 4, y - 3, 2.6, lvl === "alert" ? "#B42332" : lvl === "warn" ? "#9A5F0C" : "#2F7D4F");
      y = pdf.para(M + 14, y, t, W - 14, {size: 8.8}) + 2;
    }
    y += 6;
    y = sect(y, "Trends (one value per second; green band = reference range for the age)");
    y += 10;
    const chH = 70;
    chart(M + 22, y, W - 22, chH, X.hr, P.hr, "#B42332", "Heart rate", "bpm", 0); y += chH + 26;
    chart(M + 22, y, W - 22, chH, X.rr, P.rr, "#1D5FA8", "Breathing rate", "/min", 1); y += chH + 26;
    let tempDone = false;
    if (y + chH < 790) { chart(M + 22, y, W - 44, chH, X.tc, [35.5, 37.5], "#9A5F0C", "Temperature", "°C (left) / °F (right)", 1, true); y += chH + 26; tempDone = true; }
    /* ---------- page 2: waveforms (and the temperature trend if it did not fit) ---------- */
    if (rec.D.length > 50 || rec.R.length > 20 || !tempDone) {
      pdf.page(); y = 50;
      pdf.text(M, 34, `${rec.D.length > 50 || rec.R.length > 20 ? "Waveforms" : "Trends (continued)"} - ${rec.name}`, {size: 13, bold: true});
      if (!tempDone) { y += 14; chart(M + 22, y, W - 44, chH, X.tc, [35.5, 37.5], "#9A5F0C", "Temperature", "°C (left) / °F (right)", 1, true); y += chH + 26; }
      if (rec.D.length > 50) {
        const nS = Math.min(1000, rec.D.length), dS = nS / 100, scg = [], gcg = [];
        y = sect(y + 10, `Chest vibration (first ${dS < 10 ? dS.toFixed(1) : 10} s, 100 Hz)`);
        for (let i = 0; i < nS; i++) { scg.push(rec.D[i][0] / 100); gcg.push(rec.D[i][1] / 1000); }
        const rng = a => { const s = a.map(Math.abs).sort((p, q) => p - q); return Math.max(1e-3, (s[Math.floor(s.length * 0.995)] || 1) * 1.15); };
        const strip = (vals, col, lab, unit) => {
          const h = 80, r = rng(vals);
          pdfGrid(pdf, M, y, W, h, 1, 0.2, 0, dS);
          pdf.poly(minmaxPoints(vals, M, y, W, h, -r, r, Math.round(W * 2)), col, 0.7);
          pdf.text(M + 4, y + 10, `${lab} (+/-${r.toFixed(unit === "mg" ? 1 : 2)} ${unit})`, {size: 7.5, bold: true, color: col});
          y += h + 14;
        };
        strip(scg, "#B42332", "SCG - chest vibration", "mg");
        strip(gcg, "#6B4FA0", "GCG - chest rotation", "deg/s");
        for (let s = 0; s <= dS + 1e-6; s += dS > 4 ? 2 : 1) pdf.text(M + s / dS * W, y - 4, s + " s", {size: 6.5, color: "#5E6676", align: "center"});
        y += 10;
      }
      if (rec.R.length > 20) {
        y = sect(y, `Breathing (chest tilt, 10 Hz, ${(rec.R.length / 10).toFixed(0)} s)`);
        const vals = rec.R.map(v => v / 1000), st = statsOf(vals), pad = Math.max(0.05, (st.max - st.min) * 0.1);
        const h = 110, dur = rec.R.length / 10;
        pdfGrid(pdf, M, y, W, h, 10, 2, 0, dur);
        pdf.poly(minmaxPoints(vals, M, y, W, h, st.min - pad, st.max + pad, Math.round(W * 2)), "#1D5FA8", 0.8);
        pdf.text(M + 4, y + 10, `tilt ${st.min.toFixed(2)} to ${st.max.toFixed(2)} deg - breathing in = up`, {size: 7.5, bold: true, color: "#1D5FA8"});
        y += h + 12;
        for (let s = 0; s <= dur; s += Math.max(10, Math.round(dur / 60) * 10)) pdf.text(M + s / dur * W, y - 2, s + " s", {size: 6.5, color: "#5E6676", align: "center"});
        y += 12;
      }
      y = pdf.para(M, y + 6, "Waveform units: SCG in mg (thousandths of gravity), GCG in degrees per second, breathing as chest-wall tilt in degrees. Movement periods are included and not hidden.", W, {size: 8, color: "#5E6676"});
    }
  }
  const NPG = pdf.pages.length;
  pdf.pages.forEach((pg, i) => {
    pdf.c = pg;
    pdf.line(M, 812, M + W, 812, 0.4, "#C9B7B2");
    pdf.text(M, 824, `CardioResp Monitor - recording report generated ${dfmt(now)}`, {size: 7, color: "#5E6676"});
    pdf.text(M + W, 824, `Page ${i + 1} of ${NPG} - not a certified medical device; for a clinician to interpret`, {size: 7, color: "#5E6676", align: "right"});
  });
  return pdf.build("CardioResp recording report");
}
async function recPdf(names) {
  if (!names.length) { $("fileMsg").textContent = "No recordings to report."; return; }
  $("fileMsg").textContent = "Building the PDF report...";
  try {
    let prof = null; try { prof = await getJson("/api/profile"); } catch (e) {}
    const recs = [];
    for (const n of names) recs.push(await parseCrr(n));
    const bytes = recPdfBytes(recs, prof);
    const base = names.length === 1 ? names[0].replace(/\.crr$/, "") : "all_recordings";
    save(`CardioResp_${base}_report.pdf`, new Blob([bytes], {type: "application/pdf"}));
    $("fileMsg").textContent = `PDF report of ${names.length} recording${names.length > 1 ? "s" : ""} downloaded.`;
  } catch (e) { $("fileMsg").textContent = "PDF failed: " + (e.message || e); }
}
$("filePdfAll").onclick = () => recPdf(fileList.map(f => f.n));
/* minimal ZIP (stored, no compression) */
let crcT = null;
function crc32(b) { if (!crcT) { crcT = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; } } let c = 0xFFFFFFFF; for (let i = 0; i < b.length; i++) c = crcT[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }
function zip(entries) {
  const parts = [], cen = []; let off = 0; const enc = new TextEncoder(); const d = new Date();
  const dt = ((d.getFullYear() - 1980) << 9 | (d.getMonth() + 1) << 5 | d.getDate()) & 0xFFFF, tm = (d.getHours() << 11 | d.getMinutes() << 5 | d.getSeconds() >> 1) & 0xFFFF;
  entries.forEach(e => {
    const nm = enc.encode(e.name), crc = crc32(e.data), h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true); h.setUint16(8, 0, true); h.setUint16(10, tm, true); h.setUint16(12, dt, true);
    h.setUint32(14, crc, true); h.setUint32(18, e.data.length, true); h.setUint32(22, e.data.length, true); h.setUint16(26, nm.length, true); h.setUint16(28, 0, true);
    parts.push(new Uint8Array(h.buffer), nm, e.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x0800, true); c.setUint16(10, 0, true); c.setUint16(12, tm, true); c.setUint16(14, dt, true);
    c.setUint32(16, crc, true); c.setUint32(20, e.data.length, true); c.setUint32(24, e.data.length, true); c.setUint16(28, nm.length, true); c.setUint32(42, off, true);
    cen.push(new Uint8Array(c.buffer), nm); off += 30 + nm.length + e.data.length;
  });
  let cs = 0; cen.forEach(p => { cs += p.length; });
  const z = new DataView(new ArrayBuffer(22)); z.setUint32(0, 0x06054b50, true); z.setUint16(8, entries.length, true); z.setUint16(10, entries.length, true); z.setUint32(12, cs, true); z.setUint32(16, off, true);
  return new Blob(parts.concat(cen, [new Uint8Array(z.buffer)]));
}


/* ------------------------------ closable notifications ------------------------------ */
/* The x on a message hides it until its message changes or it ends and comes back later.
   A separate attribute is used, because the status code rewrites the class names every second. */
const NOTES = {calBanner: "calTitle", alertBanner: "alertTitle", licBanner: "licTitle", portalTip: null};
const closedSig = {};
const noteSig = id => { const t = NOTES[id] ? $(NOTES[id]) : null; return (t ? t.textContent : id).replace(/[0-9]+([.,][0-9]+)?/g, "#").trim(); };
function noteShown(e) { return e.classList.contains("show"); }
function applyClosed() {
  for (const id in NOTES) {
    const e = $(id); if (!e) continue;
    if (!noteShown(e)) { delete closedSig[id]; e.removeAttribute("data-dismissed"); continue; }
    if (closedSig[id] !== undefined && closedSig[id] !== noteSig(id)) delete closedSig[id];
    if (closedSig[id] !== undefined) e.setAttribute("data-dismissed", ""); else e.removeAttribute("data-dismissed");
  }
}
document.addEventListener("click", ev => {
  const b = ev.target.closest && ev.target.closest("[data-close]"); if (!b) return;
  const n = b.closest(".banner,.lic,.tip"); if (!n || !(n.id in NOTES)) return;
  closedSig[n.id] = noteSig(n.id); applyClosed();
});
setInterval(applyClosed, 300);
/* ------------------------------ bridge hooks ------------------------------ */
CR.onOpen = () => { syncTime(); };
CR.onStatus = onStatus;
CR.onMsg = m => {
  if (m.t === "v") { devVerdict = m; devVerdictAt = performance.now(); }
  else if (m.t === "m") renderMesh(m);
};
CR.go = go; CR.applyClosed = applyClosed; CR.toCsv = toCsv; CR.parseCrr = parseCrr; CR.recPdfBytes = recPdfBytes; CR.zip = zip; CR.crc32 = crc32; CR.zone = zone;
loadAbout();
/* ------------------------------ power / heat mode ------------------------------ */
const PWN = ["Performance", "Cool", "Eco"];
function powerInfo(S) {
  const e = $("pwInfo"); if (!e) return;
  e.innerHTML = CR.kvHtml([["Active mode", num(S.pw) ? PWN[S.pw] : "--"], ["Processor clock", num(S.mhz) ? S.mhz + " MHz" + (S.boost ? " (temporary boost)" : "") : "--"],
    ["Chip temperature", num(S.chip) ? S.chip + " \u00b0C" + (S.hot ? " (cooling down)" : "") : "--"]]);
  if (num(S.pw) && document.activeElement !== $("pwSel") && !pwTouched) $("pwSel").value = String(S.pw);
}
let pwTouched = false;
$("pwSel").onchange = () => { pwTouched = true; };
async function loadPower() {
  try { const j = await getJson("/api/power"); pwTouched = false; $("pwSel").value = String(j.pw); } catch (er) {}
}
$("pwSave").onclick = async () => {
  try { const j = await postForm("/api/power", {pw: $("pwSel").value}); pwTouched = false; $("pwMsg").textContent = "Mode set to " + PWN[j.pw] + " (" + j.mhz + " MHz)."; }
  catch (er) { $("pwMsg").textContent = "Could not change the mode: " + (er.message || er); }
};

/* ------------------------------ v1.3 alarm limits (set points and time windows), doctor / nurse PIN ------------------------------ */
const ALM_GROUPS = [
  {t: "Heart rate", u: "bpm", f: [["hrLo", "Alarm below", "w"], ["hrHi", "Alarm above", "w"], ["hrLoU", "Urgent below", "u"], ["hrHiU", "Urgent above", "u"], ["hrDly", "Delay (s)", ""]]},
  {t: "Breathing rate", u: "/min, counted over 1 min", f: [["rrLo", "Alarm below", "w"], ["rrHi", "Alarm above", "w"], ["rrLoU", "Urgent below", "u"], ["rrHiU", "Urgent above", "u"], ["rrDly", "Delay (s)", ""]]},
  {t: "Breathing pause", u: "seconds without a breath", f: [["apnea", "Alarm after (s)", "w"], ["apneaU", "Urgent after (s)", "u"]]},
  {t: "Body temperature", u: "after calibration", temp: true, f: [["tHi", "Notice above", "w"], ["tHiU", "Alarm above", "u"], ["tLo", "Alarm below", "u"], ["tDly", "Delay (s)", ""]]},
  {t: "No signal from the body", u: "sensor loose", f: [["nosigDly", "Delay (s)", ""]]}];
const ALM_RANGE = {hrLoU: [15, 200], hrLo: [20, 250], hrHi: [40, 300], hrHiU: [40, 320], rrLoU: [2, 60], rrLo: [3, 80], rrHi: [5, 120], rrHiU: [5, 150],
  apnea: [5, 60], apneaU: [5, 120], tLo: [30, 37], tHi: [36.5, 42], tHiU: [36.5, 43], hrDly: [0, 120], rrDly: [0, 120], tDly: [0, 600], nosigDly: [10, 300]};
const ALM_SHORT = {hrLo: "HR below", hrHi: "HR above", hrLoU: "HR urgent below", hrHiU: "HR urgent above", hrDly: "HR delay", rrLo: "RR below",
  rrHi: "RR above", rrLoU: "RR urgent below", rrHiU: "RR urgent above", rrDly: "RR delay", apnea: "pause", apneaU: "pause urgent", tHi: "temp notice",
  tHiU: "temp above", tLo: "temp below", tDly: "temp delay", nosigDly: "no-signal delay"};
function almDefaults(P) {                         // = alarmDefaults() in AlarmSettings.h (age-based, the built-in v1.3 rules)
  const r = Math.round;
  return {hrLo: r(0.83 * P.hr[0]), hrHi: r(1.2 * P.hr[1]), hrLoU: r(0.667 * P.hr[0]), hrHiU: r(1.5 * P.hr[1]),
    rrLo: r(0.83 * P.rr[0]), rrHi: r(1.2 * P.rr[1]), rrLoU: r(0.667 * P.rr[0]), rrHiU: r(1.5 * P.rr[1]),
    apnea: P.apnea, apneaU: Math.max(20, P.apnea), tLo: 34.5, tHi: 38.0, tHiU: 39.5, hrDly: 10, rrDly: 10, tDly: 20, nosigDly: 30};
}
const isT = k => /^t(Lo|Hi|HiU)$/.test(k);         // body-temperature set points: kept in °C, shown in °C and °F
const c2f = c => c * 9 / 5 + 32, f2c = f => (f - 32) * 5 / 9;
const bothT = c => num(c) ? `${c.toFixed(1)} °C (${c2f(c).toFixed(1)} °F)` : "--";
let almTU = "C"; try { if (localStorage.getItem("cr_tunit") === "F") almTU = "F"; } catch (e) {}
function almValidate(L) {                         // = alarmValidate() in AlarmSettings.h
  for (const k in ALM_RANGE) {
    const [lo, hi] = ALM_RANGE[k];
    if (!(num(L[k]) && L[k] >= lo - 1e-6 && L[k] <= hi + 1e-6))
      return isT(k) ? `${ALM_SHORT[k]}: enter a value from ${bothT(lo)} to ${bothT(hi)}` : `${ALM_SHORT[k]}: enter a value from ${lo} to ${hi}`;
  }
  if (!(L.hrLoU <= L.hrLo && L.hrLo < L.hrHi && L.hrHi <= L.hrHiU)) return "Heart rate: urgent below ≤ alarm below < alarm above ≤ urgent above";
  if (!(L.rrLoU <= L.rrLo && L.rrLo < L.rrHi && L.rrHi <= L.rrHiU)) return "Breathing: urgent below ≤ alarm below < alarm above ≤ urgent above";
  if (!(L.apnea <= L.apneaU)) return "Breathing pause: the urgent time must not be shorter than the alarm time";
  if (!(L.tLo < L.tHi && L.tHi <= L.tHiU)) return "Temperature: alarm below < notice above ≤ alarm above";
  return "";
}
let almCfg = null, almSeen = null, almTok = null, almEditing = false, almForm = null;
try { const t = JSON.parse(sessionStorage.getItem("cr_stok") || "null"); if (t && t.exp > Date.now()) almTok = t.tok; } catch (e) {}
function curP() { return ageProfile(lastS && num(lastS.age) ? lastS.age : (almCfg && num(almCfg.age) ? almCfg.age : 30)); }
function effLimits(P) { return !almCfg || almCfg.auto ? almDefaults(P) : Object.assign(almDefaults(P), almCfg.lim); }
function dropTok() { almTok = null; try { sessionStorage.removeItem("cr_stok"); } catch (e) {} }
async function loadAlarmCfg() {
  try {
    almCfg = await getJson("/api/alarms" + (almTok ? "?token=" + encodeURIComponent(almTok) : ""));
    if (almTok && !almCfg.signedIn) { dropTok(); if (almEditing) almStop("Signed out (30 minutes passed). Unlock again to make changes."); }
  } catch (e) { almCfg = almCfg || null; }
  renderAlm();
}
const fmtLim = (k, v) => !num(v) ? "--" : isT(k) ? bothT(v) : String(Math.round(v));
const tIn = c => !num(c) ? "" : (almTU === "F" ? c2f(c) : c).toFixed(1);      // value shown in a temperature input
const tOther = c => !num(c) ? "" : almTU === "F" ? `= ${c.toFixed(1)} °C` : `= ${c2f(c).toFixed(1)} °F`;
const limDiff = (k, a, b) => Math.abs(a - b) > (isT(k) ? 0.04 : 1e-6);
function renderAlm() {
  const P = curP(), L = effLimits(P);
  const when = t => t ? new Date(t * 1000).toLocaleString([], {day: "numeric", month: "short", hour: "2-digit", minute: "2-digit"}) : "";
  $("almSrc").innerHTML = !almCfg ? "The monitor did not send its alarm limits; the age-based defaults are used." :
    almCfg.auto ? `<b>Age-based defaults</b> for ${esc(P.name.toLowerCase())}` + (almCfg.by ? ` (chosen by ${esc(almCfg.by)}${almCfg.at ? ", " + esc(when(almCfg.at)) : ""})` : "") + ". They change with the age on the People page." :
      `<b>Set by ${esc(almCfg.by || "staff")}</b>${almCfg.at ? " on " + esc(when(almCfg.at)) : ""}. These limits stay the same when the age is changed.`;
  const src = almEditing ? almForm : L, dis = almEditing && $("almAuto").checked;
  $("almView").innerHTML = ALM_GROUPS.map(g => `<div class="limg"><h4><span class="ht">${esc(g.t)}</span> <small>${esc(g.u)}</small>` +
    (g.temp && almEditing ? `<span class="seg tus" role="group" aria-label="Temperature unit"><button type="button" data-tu="C" aria-pressed="${almTU === "C"}">°C</button><button type="button" data-tu="F" aria-pressed="${almTU === "F"}">°F</button></span>` : "") +
    `</h4><div class="lf">` + g.f.map(([k, lab, c]) => {
    const v = src[k];
    if (!almEditing) {
      if (isT(k)) return `<div><span>${esc(lab)}</span><b class="${c}">${num(v) ? v.toFixed(1) + " °C" : "--"}</b><small class="tf">${num(v) ? c2f(v).toFixed(1) + " °F" : ""}</small></div>`;
      return `<div><span>${esc(lab)}</span><b class="${c}">${fmtLim(k, v)}</b></div>`;
    }
    const chg = num(L[k]) && limDiff(k, v, L[k]) ? ' class="chg"' : "";
    if (isT(k)) {
      const [lo, hi] = ALM_RANGE[k].map(x => almTU === "F" ? c2f(x) : x);
      return `<div><label for="al_${k}"><span>${esc(lab)} (°${almTU})</span></label><input id="al_${k}" data-k="${k}" type="number" inputmode="decimal" min="${lo.toFixed(1)}" max="${hi.toFixed(1)}" step="0.1" value="${tIn(v)}"${dis ? " disabled" : ""}${chg}><small class="tf" id="alo_${k}">${tOther(v)}</small></div>`;
    }
    const [lo, hi] = ALM_RANGE[k];
    return `<div><label for="al_${k}"><span>${esc(lab)}</span></label><input id="al_${k}" data-k="${k}" type="number" inputmode="decimal" min="${lo}" max="${hi}" step="1" value="${fmtLim(k, v)}"${dis ? " disabled" : ""}${chg}></div>`;
  }).join("") + "</div></div>").join("");
  $("almBtns").hidden = almEditing || !$("almLogin").hidden;
  $("almPinWarn").hidden = !(almCfg && almCfg.pinDefault);
  const log = (almCfg && almCfg.log) || [];
  $("almLog").innerHTML = log.length ? log.map(e => `<li>${esc(when(e.t) || "time unknown")} - <b>${esc(e.by)}</b>: ${esc(e.note)}</li>`).join("") : "<li>No changes yet: the factory settings are in use.</li>";
}
function almStart() {
  almEditing = true; $("almLogin").hidden = true; $("almForm").hidden = false;
  $("almAuto").checked = !almCfg || !!almCfg.auto;
  almForm = Object.assign({}, effLimits(curP()));
  let nm = ""; try { nm = localStorage.getItem("cr_staff") || ""; } catch (e) {}
  if (!$("almBy").value) $("almBy").value = nm || ($("nuName") ? $("nuName").value : "");
  renderAlm();
}
function almStop(msg) { almEditing = false; $("almForm").hidden = true; $("almLogin").hidden = true; $("almMsg").textContent = msg || ""; renderAlm(); }
$("almView").addEventListener("input", e => {
  const i = e.target; if (!i.dataset || !i.dataset.k) return;
  const k = i.dataset.k, v = i.value === "" ? NaN : +i.value;
  almForm[k] = isT(k) && almTU === "F" && num(v) ? Math.round(f2c(v) * 100) / 100 : v;      // stored in °C
  if (isT(k)) { const o = $("alo_" + k); if (o) o.textContent = tOther(almForm[k]); }
  const L = effLimits(curP()); i.classList.toggle("chg", num(almForm[k]) && limDiff(k, almForm[k], L[k]));
});
$("almView").addEventListener("click", e => {                 // °C / °F for entering the temperature limits
  const b = e.target.closest && e.target.closest("[data-tu]"); if (!b) return;
  almTU = b.dataset.tu; try { localStorage.setItem("cr_tunit", almTU); } catch (er) {}
  renderAlm();
});
$("almEdit").onclick = () => {
  $("almMsg").textContent = "";
  if (almTok) { almStart(); return; }
  $("almLogin").hidden = false; $("almBtns").hidden = true; $("almPin").value = ""; setTimeout(() => $("almPin").focus(), 50);
};
$("almCancel").onclick = () => almStop("");
async function almUnlock() {
  const pin = $("almPin").value.trim();
  if (!/^[0-9]{4,8}$/.test(pin)) { $("almMsg").textContent = "Enter the doctor / nurse PIN (4 to 8 digits)."; return; }
  $("almUnlock").disabled = true;
  try {
    const r = await postForm("/api/alarms/login", {pin});
    almTok = r.token; try { sessionStorage.setItem("cr_stok", JSON.stringify({tok: r.token, exp: Date.now() + (r.ttl || 1800) * 1000})); } catch (e) {}
    $("almPin").value = ""; $("almMsg").textContent = "Unlocked for 30 minutes on this phone." + (r.pinDefault ? " Please change the factory PIN below." : "");
    if (almCfg) almCfg.pinDefault = r.pinDefault;
    almStart();
  } catch (e) { $("almMsg").textContent = (e.message || "Wrong PIN") + "."; }
  $("almUnlock").disabled = false;
}
$("almUnlock").onclick = almUnlock;
$("almPin").addEventListener("keydown", e => { if (e.key === "Enter") almUnlock(); });
$("almAuto").onchange = () => { if ($("almAuto").checked) almForm = almDefaults(curP()); renderAlm(); };
$("almFill").onclick = () => { $("almAuto").checked = false; almForm = almDefaults(curP()); renderAlm(); $("almMsg").textContent = "The age-based defaults are filled in. Change what is needed, then Save."; };
$("almLock").onclick = async () => { const t = almTok; dropTok(); if (t) { try { await postForm("/api/alarms/logout", {token: t}); } catch (e) {} } almStop("Locked."); };
$("almSave").onclick = async () => {
  const by = $("almBy").value.trim();
  if (by.length < 2) { $("almMsg").textContent = "Enter your name: it is kept with the change."; $("almBy").focus(); return; }
  const auto = $("almAuto").checked, P = curP(), old = effLimits(P), L = auto ? almDefaults(P) : almForm;
  const err = auto ? "" : almValidate(L);
  if (err) { $("almMsg").textContent = err + "."; return; }
  const ch = Object.keys(ALM_RANGE).filter(k => limDiff(k, L[k], old[k])).map(k => `${ALM_SHORT[k]} ${fmtLim(k, old[k])}→${fmtLim(k, L[k])}`);
  const wasAuto = !almCfg || !!almCfg.auto;
  const note = (auto !== wasAuto ? (auto ? "back to the age-based defaults" : "custom limits") + (ch.length ? "; " : "") : "") + ch.join("; ");
  if (!note) { almStop("Nothing was changed."); return; }
  const body = {token: almTok || "", by, auto: auto ? "1" : "0", t: String(Math.floor(Date.now() / 1000)), note: note.length > 119 ? note.slice(0, 118) + "\u2026" : note};
  if (!auto) for (const k in ALM_RANGE) body[k] = String(L[k]);
  $("almSave").disabled = true;
  try {
    almCfg = await postForm("/api/alarms", body); almSeen = almCfg.seq;
    try { localStorage.setItem("cr_staff", by); } catch (e) {}
    almStop("Saved. Every phone connected to this monitor now uses these limits.");
  } catch (e) {
    if (/sign in/i.test(e.message || "")) { dropTok(); almStop(""); $("almLogin").hidden = false; $("almBtns").hidden = true; $("almMsg").textContent = "Your sign-in has ended. Enter the PIN again, then save."; }
    else $("almMsg").textContent = "Not saved: " + (e.message || e) + ".";
  }
  $("almSave").disabled = false;
};
$("almPinSave").onclick = async () => {
  const a = $("almNew1").value.trim(), b = $("almNew2").value.trim();
  if (!/^[0-9]{4,8}$/.test(a)) { $("almMsg").textContent = "The new PIN must have 4 to 8 digits."; return; }
  if (a !== b) { $("almMsg").textContent = "The two PINs are different."; return; }
  try {
    await postForm("/api/alarms/pin", {token: almTok || "", new: a, by: $("almBy").value.trim(), t: String(Math.floor(Date.now() / 1000))});
    $("almNew1").value = ""; $("almNew2").value = ""; $("almMsg").textContent = "PIN changed. Tell it only to the doctors and nurses.";
    loadAlarmCfg();
  } catch (e) { $("almMsg").textContent = "PIN not changed: " + (e.message || e) + "."; }
};
loadAlarmCfg();

/* ------------------------------ v1.3 restart and factory reset ------------------------------ */
if ($("btnReboot")) $("btnReboot").onclick = async () => {
  if (!confirm("Restart the monitor now? Settings, recordings and the licence are kept. A running measurement starts again by itself.")) return;
  const b = $("btnReboot"); b.disabled = true; $("rbMsg").textContent = "Restarting...";
  try { await postForm("/api/reboot", {}); $("rbMsg").textContent = "The monitor is restarting. This page reconnects by itself in about 10 to 20 seconds."; }
  catch (e) { $("rbMsg").textContent = "Could not restart: " + (e.message || e); }
  setTimeout(() => { b.disabled = false; }, 10000);
};
if ($("btnFactory")) $("btnFactory").onclick = async () => {
  const lic = $("facLic").checked;
  if (!confirm("Factory reset: erase the Wi-Fi network, age, calibrations, temperature offset, power mode, mesh settings, patient and nurse profiles and ALL recordings" +
    (lic ? ", AND the licence key" : " (the licence key is kept)") + "? This cannot be undone.")) return;
  if (lic && !confirm("The licence key will be removed as well. Without a new activation key only heart rate, breathing rate and the placement are shown. Remove it?")) return;
  const b = $("btnFactory"); b.disabled = true; $("facMsg").textContent = "Erasing...";
  if (window.CRAlarm && CRAlarm.userStop) CRAlarm.userStop();          // the measurement stops on purpose
  try {
    const r = await postForm("/api/factory", lic ? {lic: "remove"} : {});
    $("facMsg").textContent = "Erased" + (r.lic === "removed" ? " (licence removed)" : " (licence kept)") + ". The monitor restarts now with its own Wi-Fi network: join \"" +
      ((CR.st.info && CR.st.info.dev) || "CardioResp-...") + "\" and open http://192.168.4.1";
  } catch (e) { $("facMsg").textContent = "Factory reset failed: " + (e.message || e); }
  setTimeout(() => { b.disabled = false; }, 10000);
};

/* ------------------------------ analysis drop-down ------------------------------ */
{
  const d = $("anaDrop");
  let anaOpen = false;
  try { anaOpen = localStorage.getItem("cr_ana") === "1"; } catch (er) {}
  if (d) {
    d.open = anaOpen;
    d.addEventListener("toggle", () => {
      try { localStorage.setItem("cr_ana", d.open ? "1" : "0"); } catch (er) {}
      if (d.open) setTimeout(() => { if (CR.redraw) CR.redraw(); if (CR.renderAnalysis) CR.renderAnalysis(); }, 30);
    });
  }
}
CR.onReport = () => { const r = $("reportBox"); if (r) showSec(r); if (r) setTimeout(() => r.scrollIntoView({block: "nearest"}), 50); };

go(location.hash.slice(1) || "live", false);
setInterval(() => { if (page === "mesh" && !CR.st.connected) loadMesh(); }, 5000);
})();
