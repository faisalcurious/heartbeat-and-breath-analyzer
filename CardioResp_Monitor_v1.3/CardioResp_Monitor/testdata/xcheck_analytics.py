#!/usr/bin/env python3
"""Cross-check: Python features == C++ (Analytics.h) == JavaScript (analytics.js).
Usage: python3 testdata/xcheck_analytics.py <path-to-analytics_test-binary>"""
import json, math, os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "ml"))
from cr_features import features, age_profile, forward
from cr_synth import make_status
import json as _j

binary = sys.argv[1]
rng = np.random.default_rng(5)
lines, cases = [], []
KEYS = ["hr","rr","nn","rmssd","sdnn","pnn50","hrmin","hrmax","hra","hrs","hrb","sp","pcv","ampcv","apl","apc","amp1","amp","ie","rrs","rr1","vhf","mf","tbody","win","ap5","cgh","sqh","sqr","pres","mot","post","rrn","ctot","art","tcal","pf"]
def emit(S, age, t, reset=1):
    S = dict(S)
    for k in ("hr","rr","nn","rmssd","sdnn","pnn50","hrmin","hrmax","hra","hrs","hrb","apl","apc","rrs","rr1"): 
        if S.get(k) is not None: S[k] = round(S[k], 1 if k != "nn" else 0)
    for k in ("sp",): S[k] = round(S[k], 1)
    for k in ("pcv","ampcv","amp1","amp"): S[k] = round(S[k], 3)
    S["ie"] = round(S["ie"], 2); S["vhf"] = round(S["vhf"], 2); S["mf"] = round(S["mf"], 2)
    S["sqh"] = int(S["sqh"]); S["sqr"] = int(S["sqr"]); S["ap5"] = int(S["ap5"]); S["cgh"] = int(S["cgh"])
    S.setdefault("win", 300.0); S.setdefault("mot", 0); S.setdefault("post", 0)
    S["tbody"] = S.get("tbody", float("nan"))
    parts = [f"age={age}", f"t={t}", f"reset={reset}"]
    for k in KEYS:
        v = S.get(k)
        if v is None: continue
        if isinstance(v, float) and math.isnan(v): parts.append(f"{k}=nan")
        else: parts.append(f"{k}={v}")
    lines.append(" ".join(parts)); cases.append((S, age))
for i in range(1500):
    c = int(rng.integers(0, 7))
    S, A = make_status(rng, c)
    if rng.random() < .3: S["tbody"] = float(round(rng.uniform(35.5, 39.5), 1))
    if rng.random() < .1: S["mot"] = 1
    age = {0:0.05,1:0.5,2:2,3:4,4:8,5:15,6:30,7:50,8:70}[A["id"]]
    emit(S, age, 100000 + i * 1000)
# scripted scenario: stable 90 s then heart signal vanishes while still -> arrest rule; and trend rule
base, _ = make_status(rng, 0)
base.update(hr=72.0, rr=15.0, sqh=80, sqr=80, pres=1, mf=0.0, ap5=0, apl=0, apc=0, cgh=0)
t = 1000
for k in range(60):
    emit(base, 30, t, 1 if k == 0 else 0); t += 1000
lost = dict(base); lost.update(hr=None, pres=0, sqh=5)
for k in range(30):
    emit(lost, 30, t, 0); t += 1000
trend = dict(base)
TREND_N = 900                        # v1.3: the trend rule needs 10 minutes of history
for k in range(TREND_N):
    s = dict(base); s.update(hr=72.0 + (0 if k < 700 else min(25, (k-700)*0.5)), rr=15.0 + (0 if k < 700 else min(8, (k-700)*0.1)))
    emit(s, 30, t, 1 if k == 0 else 0); t += 1000
# ---- v1.3 long scripted sessions (1 status per second) ----
SCN = {}
def session(name, n, fn, age=30):
    global t
    SCN[name] = (len(lines), n)
    for k in range(n):
        emit(fn(k), age, t, 1 if k == 0 else 0); t += 1000
calm = dict(base); calm.update(rmssd=35.0, nn=833.0, sdnn=40.0, pnn50=12.0, hra=72.0, hrs=72.0, hrb=72.0, sp=6.0, pcv=0.12, rrs=15.0, rr1=15.0, rrn=15.0, ctot=0, art=0, tcal=0, pf=0)
# A) coughs every ~2-4 minutes + calibrated fever 38.3 for 45 minutes -> chest infection pattern only after 30 calm minutes
def infect(k):
    s = dict(calm); s.update(tbody=38.3, tcal=1, rr=22.0, rrn=22.0, rr1=22.0, ctot=k // 150, cgh=1 if (k % 150) < 30 and k >= 150 else 0)
    return s
session("infect", 45 * 60, infect)
# B) the same coughs over only 10 minutes (no long-term evidence) -> must never name an infection
session("short", 10 * 60, infect)
# C) breathing pauses of 12 s every 3 minutes for 40 minutes -> sleep-apnoea pattern after 30 calm minutes
def apnoea(k):
    s = dict(calm); ph = k % 180
    if 100 <= ph < 112: s.update(apc=float(ph - 99), ap5=1, apl=12.0)
    elif ph >= 112: s.update(ap5=1, apl=12.0)
    return s
session("apnoea", 40 * 60, apnoea)
# D) irregular rhythm signature for 8 minutes -> 'lasting irregular rhythm' only after 5 minutes
def irreg(k):
    s = dict(calm); s.update(hr=88.0, hrb=88.0, hra=70.0, hrs=99.0, sp=1.5, rmssd=140.0, nn=680.0, sdnn=160.0, pnn50=70.0, hrmin=55.0, hrmax=140.0)
    return s
session("irreg", 8 * 60, irreg)
# E) fast heart + fast breathing at rest for 25 minutes -> body-wide stress signs after 20 minutes; NEWS2 high
def stress(k):
    s = dict(calm); s.update(hr=128.0, hrb=128.0, hra=128.0, hrs=128.0, nn=469.0, rr=27.0, rrs=27.0, rr1=27.0, rrn=27.0)
    return s
session("stress", 25 * 60, stress)
# F) breathing artefact: heart rate paused while breathing is forceful -> no arrest rule
def art(k):
    s = dict(calm)
    if 120 <= k < 200: s.update(hr=None, pres=1, sqh=30, art=1, rr=44.0, rrn=44.0)
    return s
session("art", 300, art)
open(os.path.join(tempfile.gettempdir(), "xc_cases.txt"), "w").write("\n".join(lines) + "\n")
path = os.path.join(tempfile.gettempdir(), "xc_cases.txt")
cpp = [json.loads(l) for l in subprocess.run([binary, path], capture_output=True, text=True, check=True).stdout.splitlines()]
js = json.loads(subprocess.run(["node", os.path.join(ROOT, "testdata", "xcheck_analytics.js"), path], capture_output=True, text=True, check=True).stdout)
assert len(cpp) == len(js) == len(cases), (len(cpp), len(js), len(cases))
# 1) python features vs js features (feature vector equality)
from_js = [j["x"] for j in js]
maxd = 0
for (S, age), x in zip(cases, from_js):
    S2 = {k: (None if (isinstance(v, float) and math.isnan(v)) else v) for k, v in S.items()}
    P = age_profile(age)
    xp = features(S2, P)
    maxd = max(maxd, max(abs(a - b) for a, b in zip(xp, x)))
print("python vs js feature max |diff|:", maxd)
assert maxd < 1e-9, maxd
# 2) C++ vs JS verdicts
bad = 0; fields = 0; worst = 0
for i, (c, j) in enumerate(zip(cpp, js)):
    j = j["v"]
    same = (c["sev"] == j["sev"] and c["code"] == j["code"] and c["mlc"] == j["mlc"] and c["head"] == j["head"] and c["cond"] == j["cond"])
    worst = max(worst, abs(c["score"] - j["score"]), max(abs(a-b) for a, b in zip(c["p"], j["p"])))
    relok = abs(c["rel"] - j["rel"]) <= 1
    if not (same and relok):
        bad += 1
        if bad <= 8: print("MISMATCH case", i, lines[i][:160], "\n  cpp:", c["sev"], c["code"], c["head"], c["rel"], "\n  js :", j["sev"], j["code"], j["head"], j["rel"])
print(f"C++ vs JS: {len(cases) - bad}/{len(cases)} identical verdicts, worst probability diff {worst:.2e}")
evs = [(c.get("ev"), j["v"].get("ev")) for c, j in zip(cpp, js)]
ev_same = sum(1 for a2, b2 in evs if [e[1] for e in (a2 or [])] == [e[1] for e in (b2 or [])])
ev_temp = [a2 for a2, b2 in evs if any("degF" in e[1] for e in (a2 or []))]
print(f"C++ vs JS evidence texts: {ev_same}/{len(evs)} identical; {len(ev_temp)} verdicts carry a temperature in degC and degF")
if ev_temp: print("  example:", [e[1] for e in ev_temp[0] if "degF" in e[1]][0])
# 3) scenario checks
codes_lost = [c["code"] for c in cpp[1500+60:1500+90]]
print("arrest rule fired in lost-heart scenario:", "arrest" in codes_lost, "(first at second", (codes_lost.index("arrest") if "arrest" in codes_lost else None), "of loss)")
codes_tr = [c["code"] for c in cpp[1500+90:1500+90+TREND_N]]
print("trend rule fired:", "trend" in codes_tr, "(first at second", (codes_tr.index("trend") if "trend" in codes_tr else None), ")")
assert "arrest" in codes_lost and "trend" in codes_tr and codes_tr.index("trend") > 600
def seg(name):
    a, n = SCN[name]; return cpp[a:a + n]
def first(name, code):
    v = [c["code"] for c in seg(name)]
    return v.index(code) if code in v else None
def anywhere(name, word):
    return any(word.lower() in (c["head"] + " " + c["cond"] + " " + " ".join(e[1] for e in c["ev"])).lower() for c in seg(name))
fi = first("infect", "resp-inf")
print("chest-infection pattern first at second", fi, "(must be >= 1800)")
assert fi is not None and fi >= 1800
assert not anywhere("short", "covid") and not anywhere("short", "infection pattern"), "an infection was named on 10 minutes of data"
assert not any("covid" in (c["head"] + c["cond"]).lower() for c in seg("infect")[:1800]), "COVID named before 30 minutes"
fo = first("apnoea", "osa"); print("sleep-apnoea pattern first at second", fo, "(must be >= 1800)")
assert fo is not None and fo >= 1800
codes_ir = [c["code"] for c in seg("irreg")]; heads_ir = [c["head"] for c in seg("irreg")]
lasting = [i for i, h in enumerate(heads_ir) if "lasting" in h]
print("irregular rhythm: noted at", codes_ir.index("irreg"), "s, 'lasting' (possible AF) from", lasting[0] if lasting else None, "s (must be >= 300)")
assert lasting and lasting[0] >= 300 and not any("fibrillation" in (c["cond"]).lower() for c in seg("irreg")[:300])
fs = first("stress", "sirs"); print("body-wide stress signs first at second", fs, "(must be >= 1200); NEWS2 max", max(c.get("news", -1) for c in seg("stress")))
assert fs is not None and fs >= 1200
assert not any(c["code"] == "arrest" for c in seg("art")), "breathing artefact raised a cardiac-arrest alarm"
print("artefact segment: no arrest rule, codes", sorted(set(c["code"] for c in seg("art"))))
sys.exit(0 if bad <= 0.01 * len(cases) else 1)
