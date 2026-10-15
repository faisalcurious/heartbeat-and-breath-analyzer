#!/usr/bin/env python3
"""Mock CardioResp v1.3 device for UI tests.
Serves web/dist.html, streams status / waves / spectra / verdict / mesh over a WebSocket on :81 and implements
the HTTP API, including the v1.3 measurement states:
   cal 4 = READY (sensor found, NOT measuring: placement preview + Start button)
   cal 1 = calibrating (shortened to 3 s here), cal 2 = measuring, cal 3 = no sensor
and the v1.3 endpoints POST /api/measure (op=start|stop), /api/reboot, /api/factory (lic=remove).
Test hooks: GET /api/set?k=<key>&v=<number>, GET /api/scn?v=<scenario>, POST /api/calls (list of received calls).
Scenarios: normal, tachy, fastbreath (strong breathing shakes the chest: art=1), nosig (sensor off the body),
           apnea (breathing pause growing), infect (long-term chest-infection verdict), lowbatt.
   python3 mock_device_v13.py [http_port]"""
import asyncio, json, sys, struct, threading, pathlib, http.server, socketserver, urllib.parse, math, time
import websockets
ROOT = pathlib.Path(__file__).resolve().parent.parent
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
CAL_SEC = 3.0
S = {"full": 1, "lic_tok": 1, "exp": 0, "t": 0, "mesh": 1, "scn": "normal", "sensor": 1, "cal": 4, "calp": 0.0, "run": 0, "sid": 1,
     "meas_t0": 0.0, "scn_t0": 0.0, "down_until": 0.0, "epoch": 0, "boot": time.time(), "tilt": 0, "mot": 0, "apc": 0, "batt": 80,
     "home": 1, "apkeep": 1}
PEERS = [{"host": "cardio", "ip": "192.168.1.40", "lbl": "Bed 2", "id": "0A0B0C"}, {"host": "cardio3", "ip": "192.168.1.61", "lbl": "", "id": "0D0E0F"}]
def wifi():
    h = S["home"]
    return {"mode": ("both" if S["apkeep"] else "home") if h else "direct", "saved": "HomeNet" if h else "", "home": h, "ip": "192.168.1.57" if h else "", "gw": "192.168.1.1",
            "mask": "255.255.255.0", "dns": "192.168.1.1", "rssi": -58, "static": 0, "host": "cardio2" if h else "cardio", "auto": 1, "uniq": "cardio-a1b2c3",
            "apKeep": S["apkeep"], "apPass": "Xy7pQ2mN9k", "ap": 1 if (S["apkeep"] or not h) else 0, "apSsid": "CardioResp-A1B2C3", "apIp": "192.168.4.1", "apCli": 0, "apOff": 0,
            "bootFailed": 0, "scanning": 0, "test": {"s": 0, "ssid": "", "ip": "", "msg": ""}}
PROF = {"patient": {"name": "A. Patient", "id": "P-100", "sex": "female", "bed": "B4", "place": "front (sternum)", "post": "lying on back", "notes": "test"},
        "nurse": {"name": "N. Nurse", "id": "N-7", "ward": "ICU", "phone": "+1 555 0100", "shift": "night"}}
def crr(n):
    H = struct.pack("<IIHf12s24s16sBB", 1760000000, 5000, 60, 35.0, b"CardioA1B2", b"A. Patient", b"P-100", 1, 1)
    out = b"H" + struct.pack("<H", len(H)) + H
    for i in range(n):
        V = struct.pack("<HHBBBBhH", 720 + i, 160, 80, 70, 0, 1, 3650, 350)
        out += b"V" + struct.pack("<H", len(V)) + V
        D = b"".join(struct.pack("<hh", i, -i) for _ in range(10))
        out += b"D" + struct.pack("<H", len(D)) + D
    out += b"E" + struct.pack("<H", 2) + struct.pack("<H", n)
    return out
FILES = {"rec0001.crr": crr(5), "rec0002.crr": crr(3)}
def lic():
    return {"en": 1, "full": S["full"], "tok": S["lic_tok"], "auth": 1, "macok": 1, "exp": S["exp"], "clk": 1, "key": 1, "mac": "AA:BB:CC:DD:EE:FF",
            "user": "Demo", "uid": "u1", "iat": 1760000000, "eat": int(time.time()) + 200 * 86400, "now": int(time.time()), "msg": "ok"}
W = {"d": 0, "r": 0, "nextbeat": 30}
def waves():
    """50 display samples (100 Hz) + 5 breathing samples (10 Hz) per 0.5 s, like the firmware"""
    mot = S["mot"]; fast = S["scn"] == "fastbreath"; nosig = S["scn"] == "nosig"
    a = []; g = []; e = []; b = []
    for k in range(50):
        i = W["d"] + k
        v = 0
        if i >= W["nextbeat"] and not nosig:
            ph = i - W["nextbeat"]
            v = 900 * math.exp(-ph / 6.0) * math.sin(ph * 0.9)
            if ph > 40:
                ibi = 83 + int(round(5 * math.sin(2 * math.pi * W["nextbeat"] / 400.0)))
                b.append([W["nextbeat"], ibi * 10, (8 if fast else 1) | (2 if mot else 0)]); W["nextbeat"] += ibi
        amp = 9 if mot else 1
        a.append(int(v * amp + (500 * math.sin(i * 2 * math.pi / 133) if fast else 0))); g.append(int(v * 0.5 * amp)); e.append(int(abs(v)))
    r = []
    per = 13 if fast else 40                                  # samples per breath at 10 Hz (46 /min or 15 /min)
    for k in range(5):
        i = W["r"] + k
        val = 0 if nosig else int((2500 if fast else 1500) * math.sin(i * 2 * math.pi / per)) + (int(4000 * math.sin(i * 0.9)) if mot else 0)
        r.append("%d%s" % (val, ".5" if mot else ""))
    br = []
    for k in range(5):
        i = W["r"] + k
        if i > 0 and i % per == 0 and not nosig:
            n = i // per; pms = per * 100
            br.append([i, pms, int(pms * 0.42), int(pms * 0.58), 1000 + int(150 * math.sin(n * 1.3)), 1 | (2 if mot else 0), i - per // 2])
    m = '{"t":"w","sid":%d,"i":%d,"a":%s,"g":%s,"e":%s,"ri":%d,"r":[%s],"b":%s,"br":%s,"m":%d}' % (S["sid"], W["d"], json.dumps(a), json.dumps(g), json.dumps(e), W["r"], ",".join(r), json.dumps(b), json.dumps(br), mot)
    W["d"] += 50; W["r"] += 5
    return m
def spectra():
    hr = 72.0 / 60; rr = 16.0 / 60
    cd = 4.0 / 164; rd = 1.25 / 128; vd = 50.0 / 128
    gs = lambda f, f0, w: math.exp(-((f - f0) / w) ** 2)
    c = [int(1000 * (gs(i * cd, hr, 0.05) + 0.35 * gs(i * cd, 2 * hr, 0.06)) + 15) for i in range(164)]
    r = [int(1000 * (gs(i * rd, rr, 0.02) + 0.1 * gs(i * rd, 2 * rr, 0.03)) + 10) for i in range(128)]
    va = [int(-12 - 30 * (1 - gs(i * vd, 15, 9)) - 0.25 * i * vd) for i in range(128)]
    vg = [int(-18 - 28 * (1 - gs(i * vd, 12, 8)) - 0.3 * i * vd) for i in range(128)]
    return json.dumps({"t": "f", "cd": cd, "cp": hr, "c": c, "rd": rd, "rp": rr, "r": r, "vd": vd, "va": va, "vg": vg})
def orient(measuring):
    a = math.radians(S["tilt"]); gv = [0.0, round(math.sin(a), 3), round(math.cos(a), 3)]
    o = {"g": gv, "roll": round(math.degrees(math.atan2(gv[1], gv[2])), 1), "pitch": 0.0, "tilt": float(S["tilt"]) if (measuring or S["epoch"] or S["sid"] > 1) else None,
         "post": 1 if (measuring and S["tilt"] > 30) else 0, "mot": S["mot"], "mf": 0.0 if measuring else None,
         "still": 0 if S["mot"] else (100 if measuring else 96), "osrc": 1 if measuring else 2}
    return o
def now_s(): return time.time() - S["boot"]
def base():
    return {"t": "s", "sid": S["sid"], "up": int(now_s() * 1000), "cal": S["cal"], "calp": round(S["calp"], 2), "cala": 1,
            "calmsg": {4: "Sensor ready - place it on the chest and press Start", 1: "Calibrating - keep still and breathe normally", 2: "Calibrated: |g| = 1.002 g, tilt reference stored",
                       3: "Motion sensor not found - check the sensor cable and connections", 0: ""}[S["cal"]],
            "age": 70, "run": S["run"], "alv": ALM["seq"], "wfs": 1, "led": 1, "btn": 1, "tcal": 1, "full": S["full"], "lic": lic(),
            "tb": 36.9, "chip": 48, "hot": 0, "pw": 1, "mhz": 80, "boost": 0, "rec": {"on": 0, "s": 0, "t": 0, "n": ""}, "mesh": S["mesh"], "mn": 2,
            "bat": {"ok": 1, "v": 3.5 + S["batt"] / 200.0, "p": int(S["batt"])}, "temp": 31.0, "heap": 150000, "minheap": 120000, "cli": 1,
            "net": {"home": S["home"], "ap": 1, "ssid": "HomeNet" if S["home"] else "", "host": "cardio2" if S["home"] else "cardio", "ip": "192.168.1.57" if S["home"] else "", "rssi": -58}, "ovf": 0, "i2c": 0, "au": 20, "aumax": 40}
def vit():
    t = now_s(); sc = S["scn"]; ts = t - S["scn_t0"]
    v = {"hr": 72 + 3 * math.sin(t / 5), "rr": 16 + math.sin(t / 7), "rr1": 16.0, "rrn": 16.0, "b1m": 16, "pres": 1, "sqh": 82, "sqr": 78, "art": 0, "brart": 0.08, "apc": float(S["apc"])}
    if sc == "tachy": v.update(hr=132.0, rr=28.0, rr1=28.0, rrn=28.0, b1m=28)
    if sc == "fastbreath":                                     # 46 /min: the newest breaths show it at once, the 1-min count follows
        r1 = min(46.0, 16 + 30 * ts / 60.0)
        v.update(hr=88.0, rrn=46.0, rr1=r1, rr=min(46.0, 16 + 30 * ts / 300.0), b1m=int(r1), art=1, brart=0.55, sqh=30)
    if sc == "nosig": v.update(hr=None, rr=None, rr1=None, rrn=None, b1m=0, pres=0, sqh=4, sqr=5)
    if sc == "apnea": v.update(apc=float(int(ts)), rrn=None if ts > 12 else 14.0)
    return v
def status():
    m = base()
    S["batt"] = 4 if S["scn"] == "lowbatt" else 80
    if S["cal"] != 2:
        if S["sensor"]: m.update(orient(False))
        return m
    v = vit(); win = min(300, int(now_s() - S["meas_t0"]))
    o = orient(True)
    if S["full"]:
        hr = v["hr"]
        m.update({"hr": hr, "hri": None if hr is None else hr + 1, "hra": hr, "hrs": hr, "hrb": hr, "sqh": v["sqh"], "pres": v["pres"],
                  "nn": None if hr is None else round(60000 / hr), "sdnn": 45.0, "rmssd": 38.0, "pnn50": 14.0, "hrmin": None if hr is None else hr - 4, "hrmax": None if hr is None else hr + 4,
                  "b60": 70, "v60": 60 if v["pres"] else 0, "cov": 0.9, "qa": 0.8, "qg": 0.6, "qf": 0.7, "wa": 0.6, "wg": 0.4, "snr": 6.0, "sp": 6.0,
                  "rr": v["rr"], "rr1": v["rr1"], "rrs": v["rr"], "sqr": v["sqr"], "amp": 1.0, "amp1": 1.0, "ampcv": 0.2, "pcv": 0.1, "ti": 1.5, "te": 2.0, "ie": 0.75,
                  "n5": 80, "ap5": 0, "apl": 0, "apc": v["apc"], "sig": 1, "win": win, "pol": 1, "rsp": 9.0,
                  "rrn": v["rrn"], "b1m": v["b1m"], "btot": 120, "ctot": 2, "art": v["art"], "brart": v["brart"],
                  "vc": [1, 0, 0], "vg": [0, 1, 0], "vr": [0, 0, 1], "ab": [0, 0, 0], "pf": 0, "fs": 200.0, "gap": 0, "bias": [0.1, -0.2, 0.05], "ag": 8,
                  "now": int(now_s() * 1000), "vra": 12.0, "vrg": 0.8, "vpk": 40.0, "vhf": 0.1, "cgh": 0, "tbody": 36.9})
        m.update({k: o[k] for k in ("g", "roll", "pitch", "tilt", "post", "mot", "mf")}); m["still"] = o["still"]; m["osrc"] = 1
    else:
        m.update({"hr": v["hr"], "hri": v["hr"], "sqh": v["sqh"], "pres": v["pres"], "rr": v["rr"], "rr1": v["rr1"], "rrn": v["rrn"], "b1m": v["b1m"], "sqr": v["sqr"],
                  "win": win, "pf": 0, "apc": v["apc"], "art": v["art"], "ag": 8, "now": int(now_s() * 1000)})
        m.update(o)
    return m
def verdict():
    sc = S["scn"]
    obs = min(60, int((now_s() - S["meas_t0"]) / 60))
    if sc == "infect":
        return {"t": "v", "sev": 2, "code": "resp-inf", "head": "Chest infection pattern", "cond": "Pattern compatible with a chest infection (for example flu, COVID-19 or pneumonia) - a test and a doctor's review are needed",
                "score": 0.81, "mlc": 6, "mln": "coughing", "rel": 74, "agree": 1, "p": [0.1, 0.02, 0, 0.02, 0.04, 0.01, 0.81],
                "ev": [["w", "Over 35 calm minutes: 18 coughs in 5 separate 5-minute periods, raised temperature."]], "miss": [], "obs": 35, "need": 30, "news": 3}
    if sc == "tachy":
        return {"t": "v", "sev": 2, "code": "tachy", "head": "Heart rate moderately above range", "cond": "Fast heart rate (132 bpm)", "score": 0.91, "mlc": 1, "mln": "tachycardia",
                "rel": 78, "agree": 1, "p": [0.03, 0.91, 0, 0.02, 0.03, 0, 0.01], "ev": [["a", "Heart rate 132 bpm is moderately above the age range 60-100 bpm."]], "miss": [], "obs": obs, "need": 30, "news": 6}
    return {"t": "v", "sev": 0, "code": "ok", "head": "No abnormality detected", "cond": "Heart and breathing pattern within the expected range", "score": 0.97, "mlc": 0, "mln": "normal",
            "rel": 85, "agree": 1, "p": [0.97, 0.01, 0, 0.01, 0.01, 0, 0], "ev": [["o", "Heart rate 72 bpm is inside the age range 60-100 bpm."]],
            "miss": ["Long-term pattern checks (infection, sleep apnoea, lasting irregular rhythm) need 30 calm minutes: %d so far." % obs], "obs": obs, "need": 30, "news": 0}
def mesh():
    nd = [{"id": "A1B2C3", "name": "CardioA1B2", "hr": 72, "rr": 16, "sqh": 80, "sqr": 75, "sev": 0, "rel": 85, "mlc": 0, "mls": 0.97, "fl": 29, "age": 35, "t": 36.8, "rssi": 0, "hops": 0, "ago": 0, "up": 100, "self": 1, "lbl": "Bed 4"}]
    return {"t": "m", "on": S["mesh"], "up": 1, "ttl": 2, "ver": 1, "grp": "ward", "open": 0, "lbl": "Bed 4", "per": 2000, "me": "A1B2C3", "ch": 6, "nodes": nd if S["mesh"] else [], "near": []}
def mcfg():
    return {"on": S["mesh"], "grp": "ward", "pinSet": 1, "pinLen": 4, "lbl": "Bed 4", "per": 2000, "ttl": 2, "id": "A1B2C3", "ch": 6}
CALLS = []
# ---- v1.3 alarm limits (same rules as CardioRespMonitor/AlarmSettings.h)
import secrets
def alm_defaults():
    hr, rr, apnea = (60, 100), (12, 20), 10                       # the mock patient is 70 years old
    r = lambda v: int(v + 0.5)
    return {"hrLo": r(0.83 * hr[0]), "hrHi": r(1.2 * hr[1]), "hrLoU": r(0.667 * hr[0]), "hrHiU": r(1.5 * hr[1]), "rrLo": r(0.83 * rr[0]), "rrHi": r(1.2 * rr[1]),
            "rrLoU": r(0.667 * rr[0]), "rrHiU": r(1.5 * rr[1]), "apnea": apnea, "apneaU": max(20, apnea), "tLo": 34.5, "tHi": 38.0, "tHiU": 39.5,
            "hrDly": 10, "rrDly": 10, "tDly": 20, "nosigDly": 30}
ALM_R = {"hrLoU": (15, 200), "hrLo": (20, 250), "hrHi": (40, 300), "hrHiU": (40, 320), "rrLoU": (2, 60), "rrLo": (3, 80), "rrHi": (5, 120), "rrHiU": (5, 150),
         "apnea": (5, 60), "apneaU": (5, 120), "tLo": (30, 37), "tHi": (36.5, 42), "tHiU": (36.5, 43), "hrDly": (0, 120), "rrDly": (0, 120), "tDly": (0, 600), "nosigDly": (10, 300)}
def alm_validate(L):
    for k, (lo, hi) in ALM_R.items():
        v = L.get(k)
        if not (isinstance(v, float) or isinstance(v, int)) or v != v or not (lo <= v <= hi): return "%s must be between %g and %g" % (k, lo, hi)
    if not (L["hrLoU"] <= L["hrLo"] < L["hrHi"] <= L["hrHiU"]): return "Heart rate: urgent-low <= low < high <= urgent-high"
    if not (L["rrLoU"] <= L["rrLo"] < L["rrHi"] <= L["rrHiU"]): return "Breathing: urgent-low <= low < high <= urgent-high"
    if not (L["apnea"] <= L["apneaU"]): return "Breathing pause: the urgent time must not be shorter than the warning time"
    if not (L["tLo"] < L["tHi"] <= L["tHiU"]): return "Temperature: low < high <= urgent-high"
    return ""
def alm_reset():
    return {"auto": 1, "lim": None, "seq": 0, "at": 0, "by": "", "pin": "1234", "pinDefault": 1, "log": [], "wrong": 0, "lockUntil": 0.0, "lockS": 60, "tokens": {}}
ALM = alm_reset()
def alm_tok(form):
    t = form.get("token", "")
    return t if t and ALM["tokens"].get(t, 0) > time.time() else None
def alm_json(q_token=None):
    return {"auto": ALM["auto"], "seq": ALM["seq"], "at": ALM["at"], "by": ALM["by"], "lim": ALM["lim"] if (ALM["lim"] and not ALM["auto"]) else alm_defaults(),
            "def": alm_defaults(), "group": "Older adult (65+ years)", "age": 70, "pinDefault": ALM["pinDefault"],
            "locked": max(0, int(ALM["lockUntil"] - time.time() + 0.999)), "signedIn": 1 if (q_token and ALM["tokens"].get(q_token, 0) > time.time()) else 0, "log": ALM["log"][:8]}
def do_start():
    if not S["sensor"]: return False
    S["cal"] = 1; S["calp"] = 0.0; S["run"] = 1
    return True
def do_stop():
    S["cal"] = 4 if S["sensor"] else 3; S["run"] = 0; S["calp"] = 0.0
def restart(clear=False, remove_lic=False):
    S["down_until"] = time.time() + 2.5; S["epoch"] += 1; S["boot"] = time.time() + 2.5
    if clear: S["run"] = 0; S["tilt"] = 0
    if remove_lic: S["full"] = 0; S["lic_tok"] = 0
    if S["sensor"]:
        if S["run"]: S["cal"] = 1; S["calp"] = 0.0
        else: S["cal"] = 4
    else: S["cal"] = 3
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _s(self, code, body, ct="application/json"):
        if not isinstance(body, bytes): body = json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); p = u.path; q = urllib.parse.parse_qs(u.query)
        if p in ("/", "/index.html"): return self._s(200, (ROOT / "web/dist.html").read_bytes(), "text/html; charset=utf-8")
        if time.time() < S["down_until"]: return self._s(503, {"error": "restarting"})
        if p == "/api/license": return self._s(200, lic())
        if p == "/api/about": return self._s(200, {"name": "Test Designer", "email": "d@example.com", "phone": "+92 300 1234567", "wa": "923001234567", "org": "TestOrg", "fw": "1.3.0", "build": "x", "ml": "baseline", "dev": "CardioA1B2", "mac": "AA:BB:CC:DD:EE:FF", "flash": 4194304, "sketch": 1450000, "freesketch": 1769472, "cpu": 160, "ota": "open"})
        if p == "/api/profile": return self._s(200, PROF)
        if p == "/api/files": return self._s(200, {"ok": 1, "free": 500000, "total": 1000000, "rec": 0, "sec": 0, "tot": 0, "cur": "", "files": [{"n": k, "s": len(v)} for k, v in FILES.items()]})
        if p == "/api/file":
            n = q.get("n", [""])[0]
            return self._s(200, FILES[n], "application/octet-stream") if n in FILES else self._s(404, b"nf", "text/plain")
        if p == "/api/mesh": return self._s(200, mesh())
        if p == "/api/mesh/config": return self._s(200, mcfg())
        if p == "/api/status": return self._s(200, status())
        if p == "/api/wifi": return self._s(200, wifi())
        if p == "/api/wifi/scan": return self._s(200, {"running": 0, "nets": [["HomeNet", -55, 0], ["Neighbour", -78, 0]]})
        if p == "/api/peers":
            if q.get("scan", [""])[0] == "1": S["peer_t"] = time.time()
            run = time.time() - S.get("peer_t", 0) < 1.5
            return self._s(200, {"running": 1 if run else 0, "age": 0, "me": "cardio2" if S["home"] else "cardio", "home": S["home"], "peers": [] if (run or not S["home"]) else PEERS})
        if p == "/api/log":
            if S["cal"] != 2: return self._s(503, {"error": "measurement not running"})
            return self._s(200, json.loads((ROOT / "testdata/log.json").read_text()))
        if p == "/api/power": return self._s(200, {"pw": 1, "mhz": 80, "chip": 48, "hot": 0, "boost": 0})
        if p == "/api/set":
            k = q["k"][0]; v = float(q["v"][0]); S[k] = int(v) if v == int(v) else v
            if k == "sensor": S["cal"] = (4 if not S["run"] else 1) if v else 3
            return self._s(200, {"ok": 1})
        if p == "/api/scn": S["scn"] = q["v"][0]; S["scn_t0"] = now_s(); return self._s(200, {"ok": 1})
        if p == "/api/state": return self._s(200, {k: S[k] for k in ("cal", "run", "full", "sid", "scn", "epoch")})
        if p == "/api/alarms": return self._s(200, alm_json(q.get("token", [""])[0]))
        if p == "/api/almstate": return self._s(200, {k: ALM[k] for k in ("auto", "lim", "seq", "by", "pin", "pinDefault", "log")})
        if p == "/api/almreset": ALM["wrong"] = 0; ALM["lockUntil"] = 0.0; ALM["lockS"] = 60; return self._s(200, {"ok": 1})
        self._s(404, {})
    def do_POST(self):
        u = urllib.parse.urlparse(self.path); p = u.path
        n = int(self.headers.get("Content-Length", 0) or 0); raw = self.rfile.read(n)
        form = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode("latin1") if "urlencoded" in self.headers.get("Content-Type", "") else "").items()}
        if p == "/api/calls": return self._s(200, [[a, b, c] for a, b, c in CALLS])
        CALLS.append((p, form, len(raw)))
        if time.time() < S["down_until"]: return self._s(503, {"error": "restarting"})
        if p == "/api/measure":
            op = form.get("op", "")
            if op == "start": return self._s(200, {"ok": 1, "op": "start"}) if do_start() else self._s(409, {"error": "The motion sensor is not connected"})
            if op == "stop": do_stop(); return self._s(200, {"ok": 1, "op": "stop"})
            return self._s(400, {"error": "op must be start or stop"})
        if p == "/api/reboot": self._s(200, {"ok": 1}); restart(); return
        if p == "/api/alarms/login":
            now = time.time()
            if ALM["lockUntil"] > now: w = int(ALM["lockUntil"] - now + 0.999); return self._s(429, {"error": "Too many wrong PINs - wait %d s" % w, "locked": w})
            if form.get("pin", "") != ALM["pin"]:
                ALM["wrong"] += 1
                if ALM["wrong"] >= 5: ALM["lockUntil"] = now + ALM["lockS"]; ALM["lockS"] = min(900, ALM["lockS"] * 2); ALM["wrong"] = 0
                return self._s(403, {"error": "Wrong PIN"})
            ALM["wrong"] = 0; ALM["lockS"] = 60
            t = secrets.token_hex(16); ALM["tokens"][t] = now + 1800
            return self._s(200, {"ok": 1, "token": t, "ttl": 1800, "pinDefault": ALM["pinDefault"]})
        if p == "/api/alarms/logout": ALM["tokens"].pop(form.get("token", ""), None); return self._s(200, {"ok": 1})
        if p == "/api/alarms":
            if not alm_tok(form): return self._s(401, {"error": "Sign in with the doctor / nurse PIN first"})
            by = form.get("by", "").strip()
            if len(by) < 2: return self._s(400, {"error": "Enter your name (it is kept with the change)"})
            auto = 1 if form.get("auto") == "1" else 0
            if not auto:
                L = dict(ALM["lim"] if (ALM["lim"] and not ALM["auto"]) else alm_defaults())
                for k in ALM_R:
                    if k in form:
                        try: L[k] = float(form[k])
                        except ValueError: L[k] = float("nan")
                err = alm_validate(L)
                if err: return self._s(400, {"error": err})
                ALM["lim"] = L
            ALM["auto"] = auto; ALM["seq"] += 1; ALM["by"] = by; ALM["at"] = int(form.get("t", "0") or 0)
            ALM["log"].insert(0, {"t": ALM["at"], "by": by, "note": form.get("note", "") or ("age-based defaults" if auto else "limits changed")})
            return self._s(200, alm_json(form.get("token")))
        if p == "/api/alarms/pin":
            if not alm_tok(form): return self._s(401, {"error": "Sign in with the doctor / nurse PIN first"})
            nw = form.get("new", "")
            if not (nw.isdigit() and 4 <= len(nw) <= 8): return self._s(400, {"error": "The PIN must have 4 to 8 digits"})
            ALM["pin"] = nw; ALM["pinDefault"] = 0
            ALM["log"].insert(0, {"t": int(form.get("t", "0") or 0), "by": form.get("by", "") or "staff", "note": "doctor / nurse PIN changed"})
            return self._s(200, {"ok": 1})
        if p == "/api/factory":
            rm = form.get("lic") == "remove"
            ALM.clear(); ALM.update(alm_reset())
            self._s(200, {"ok": 1, "lic": "removed" if rm else "kept"}); restart(clear=True, remove_lic=rm); return
        if p == "/api/license":
            if form.get("token", "") == "good-token-" + "x" * 40: S.update(full=1, exp=0, lic_tok=1); return self._s(200, dict(lic(), ok=1, msg="Activated"))
            return self._s(400, dict(lic(), ok=0, msg="Not valid"))
        if p == "/api/license/remove": S.update(full=0, lic_tok=0); return self._s(200, lic())
        if p == "/api/profile":
            k = form.pop("kind", ""); PROF[k].update({a: b for a, b in form.items() if a in PROF[k]}); return self._s(200, PROF)
        if p == "/api/file/delete": FILES.pop(form.get("n"), None); return self._s(200, {"ok": 1})
        if p == "/api/rec": return self._s(200, {"ok": 1, "name": "rec0003.crr", "sec": 60})
        if p == "/api/mesh": return self._s(200, mesh())
        if p == "/api/mesh/config": return self._s(200, mcfg())
        if p == "/api/temp": return self._s(200, {"ok": 1, "offset": 5.25})
        if p == "/api/power": return self._s(200, {"pw": 1, "mhz": 80, "chip": 48, "hot": 0, "boost": 0})
        if p == "/api/time": return self._s(200, {"ok": 1})
        if p == "/api/wifi/apkeep": S["apkeep"] = 1 if form.get("on") == "1" else 0; return self._s(200, wifi())
        if p == "/api/update": return self._s(200, {"ok": 1, "bytes": n, "kind": "app"})
        self._s(404, {})
def http_thread():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("0.0.0.0", PORT), H) as s: s.serve_forever()
INFO = {"t": "i", "dev": "CardioA1B2", "host": "cardio", "fw": "1.3.0", "sensor": 1, "st": {"ran": 1, "pass": 1, "a": [1, 1, 1], "g": [1, 1, 1]},
        "calib": {"ok": 1, "att": 1, "gm": 1.0, "bias": [0, 0, 0], "g0": [0, 0.0, 1.0], "astd": [0, 0, 0], "gstd": [0, 0, 0], "drift": 0.1}, "age": 70, "group": "Older adult (65+ years)",
        "fsn": 200, "wsport": 81, "sda": 8, "scl": 9, "ledpin": -1, "btnpin": -1, "wfs": 1}
def tick():
    """device state machine, called every 0.5 s"""
    if not S["sensor"]: S["cal"] = 3; return
    if S["cal"] == 3 and S["sensor"]: S["cal"] = 1 if S["run"] else 4
    if S["cal"] == 1:
        if not S["mot"]: S["calp"] = min(1.0, S["calp"] + 0.5 / CAL_SEC)
        if S["calp"] >= 1.0:
            S["cal"] = 2; S["sid"] += 1; S["meas_t0"] = now_s(); S["scn_t0"] = now_s()
async def handler(ws):
    if time.time() < S["down_until"]: await ws.close(); return
    ep = S["epoch"]
    await ws.send(json.dumps(INFO))
    async def rd():
        async for m in ws:
            CALLS.append(("ws", m, 0))
            try: c = json.loads(m)
            except Exception: continue
            cmd = c.get("cmd")
            if cmd == "start": do_start()
            elif cmd == "stop": do_stop()
            elif cmd in ("recal", "reset") and S["cal"] == 4: do_start()
            elif cmd == "recal": S["cal"] = 1; S["calp"] = 0.0
    t = asyncio.create_task(rd())
    k = 0
    try:
        while S["epoch"] == ep:
            k += 1
            await ws.send(json.dumps(status()))
            if S["cal"] == 2:
                await ws.send(waves())
                if S["full"]:
                    if k % 4 == 1: await ws.send(spectra())
                    await ws.send(json.dumps(verdict()))
            if k % 4 == 0: await ws.send(json.dumps(mesh()))
            await asyncio.sleep(0.5)
        await ws.close()
    except Exception: pass
    finally: t.cancel()
async def ticker():
    while True:
        tick(); await asyncio.sleep(0.5)
async def main():
    threading.Thread(target=http_thread, daemon=True).start()
    asyncio.create_task(ticker())
    async with websockets.serve(handler, "0.0.0.0", 81, max_size=None):
        print("mock v1.3 http://localhost:%d" % PORT, flush=True); await asyncio.Future()
asyncio.run(main())
