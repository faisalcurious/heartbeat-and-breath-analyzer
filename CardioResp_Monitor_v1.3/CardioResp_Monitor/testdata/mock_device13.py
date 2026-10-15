#!/usr/bin/env python3
"""Mock CardioResp v1.2 device for UI tests: serves web/dist.html, streams 1 Hz status + verdict + mesh over a
WebSocket on :81 and implements the v1.2 HTTP API (licence, profile, files, rec, mesh, temp, update).
   python3 mock_device12.py [http_port]"""
import asyncio, json, sys, struct, threading, pathlib, http.server, socketserver, urllib.parse, math, time
import websockets
ROOT = pathlib.Path(__file__).resolve().parent.parent
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
S = {"full": 1, "lic_tok": 1, "exp": 0, "t": 0, "mesh": 1, "scn": "normal"}
PROF = {"patient": {"name": "A. Patient", "id": "P-100", "sex": "female", "bed": "B4", "place": "front (sternum)", "post": "lying on back", "notes": "test"},
        "nurse": {"name": "N. Nurse", "id": "N-7", "ward": "ICU", "phone": "+1 555 0100", "shift": "night"}}
LOG = []
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
def crr_rich(n=60):
    H = struct.pack("<IIHf12s24s16sBB", 1760000000, 5000, n, 35.0, b"CardioA1B2", b"A. Patient", b"P-100", 1, 1)
    out = b"H" + struct.pack("<H", len(H)) + H
    for i in range(n):
        hr = 72 + 4 * math.sin(i / 6) + (40 if 30 <= i < 42 else 0)
        rr = 15 + math.sin(i / 9) + (9 if 30 <= i < 42 else 0)
        sev = 2 if 32 <= i < 42 else (1 if 28 <= i < 32 else 0)
        mot = 1 if 50 <= i < 54 else 0
        fl = 1 | (mot << 1) | 4                     # bit 2 = assessment ran
        tc = 3650 if i < 20 else 3650 + (i - 20) * 4
        V = struct.pack("<HHBBBBhH", int(hr * 10), int(rr * 10), 80 - 30 * mot, 72, sev, fl, int(tc), 380)
        out += b"V" + struct.pack("<H", len(V)) + V
        D = b""
        for k in range(100):
            t = i + k / 100.0; ph = (t * hr / 60.0) % 1.0
            v = 900 * math.exp(-ph * 60 / 6.0 * 0.1) * math.sin(ph * 40) if ph < 0.25 else 15 * math.sin(k)
            if mot: v *= 6
            D += struct.pack("<hh", int(v), int(v * 0.4))
        for c in range(0, len(D), 256): out += b"D" + struct.pack("<H", len(D[c:c+256])) + D[c:c+256]
        R = b"".join(struct.pack("<h", int(1200 * math.sin(2 * math.pi * (i + k / 10.0) * rr / 60.0) + (3000 * math.sin(k) if mot else 0))) for k in range(10))
        out += b"R" + struct.pack("<H", len(R)) + R
    out += b"E" + struct.pack("<H", 2) + struct.pack("<H", n)
    return out
FILES = {"rec0001.crr": crr(5), "rec0002.crr": crr(3), "rec0003.crr": crr_rich(60)}
def lic():
    return {"en": 1, "full": S["full"], "tok": S["lic_tok"], "auth": 1, "macok": 1, "exp": S["exp"], "clk": 1, "key": 1, "mac": "AA:BB:CC:DD:EE:FF",
            "user": "Demo", "uid": "u1", "iat": 1760000000, "eat": 1790000000, "now": int(time.time()), "msg": "ok"}
def tiltg(t):
    a = math.radians(S.get("tilt", 0)); return [0, round(math.sin(a), 3), round(math.cos(a), 3)]
W = {"d": 0, "r": 0, "beat": 0, "nextbeat": 30}
def waves():
    """50 display samples (100 Hz) + 5 breathing samples (10 Hz) per 0.5 s. Movement: huge heart amplitude, resp flagged .5"""
    mot = S.get("mot", 0)
    a = []; g = []; e = []; b = []
    for k in range(50):
        i = W["d"] + k
        ph = (i - W["nextbeat"])
        v = 0
        if i >= W["nextbeat"]:
            ph = i - W["nextbeat"]
            v = 900 * math.exp(-ph / 6.0) * math.sin(ph * 0.9)
            if ph > 40:                                   # beat done: marker at its onset, next interval follows breathing (sinus arrhythmia)
                ibi = 83 + int(round(5 * math.sin(2 * math.pi * W["nextbeat"] / 400.0) + 2 * math.sin(W["nextbeat"] * 0.37)))
                b.append([W["nextbeat"], ibi * 10, 1 | (2 if mot else 0)]); W["nextbeat"] += ibi
        amp = 9 if mot else 1
        a.append(int(v * amp)); g.append(int(v * 0.5 * amp)); e.append(int(abs(v)))
    r = []
    for k in range(5):
        i = W["r"] + k
        val = int(1500 * math.sin(i * 2 * math.pi / 40)) + (int(4000 * math.sin(i * 0.9)) if mot else 0)
        r.append("%d%s" % (val, ".5" if mot else ""))
    br = []
    for k in range(5):                                    # one breath every 40 samples (4 s), depth varies a little, a sigh now and then
        i = W["r"] + k
        if i > 0 and i % 40 == 0:
            n = i // 40; per = 3800 + int(300 * math.sin(n * 0.7)); amp = 1000 + int(150 * math.sin(n * 1.3)) + (1200 if n % 23 == 0 else 0)
            br.append([i, per, int(per * 0.42), int(per * 0.58), amp, 1 | (2 if mot else 0) | (4 if n % 23 == 0 else 0), i - 24])
    m = '{"t":"w","sid":1,"i":%d,"a":%s,"g":%s,"e":%s,"ri":%d,"r":[%s],"b":%s,"br":%s,"m":%d}' % (W["d"], json.dumps(a), json.dumps(g), json.dumps(e), W["r"], ",".join(r), json.dumps(b), json.dumps(br), mot)
    W["d"] += 50; W["r"] += 5
    return m
def spectra():
    """synthetic spectra like protoSpectra: heart rhythm peak + harmonics, breathing peak, vibration in dB"""
    hr = 72.0 / 60; rr = 16.0 / 60
    cd = 4.0 / 164; rd = 1.25 / 128; vd = 50.0 / 128
    g = lambda f, f0, w: math.exp(-((f - f0) / w) ** 2)
    c = [int(1000 * (g(i * cd, hr, 0.05) + 0.35 * g(i * cd, 2 * hr, 0.06) + 0.12 * g(i * cd, 3 * hr, 0.07)) + 15) for i in range(164)]
    r = [int(1000 * (g(i * rd, rr, 0.02) + 0.1 * g(i * rd, 2 * rr, 0.03)) + 10) for i in range(128)]
    va = [int(-12 - 30 * (1 - g(i * vd, 15, 9)) - 0.25 * i * vd) for i in range(128)]
    vg = [int(-18 - 28 * (1 - g(i * vd, 12, 8)) - 0.3 * i * vd) for i in range(128)]
    return json.dumps({"t": "f", "cd": cd, "cp": hr, "c": c, "rd": rd, "rp": rr, "r": r, "vd": vd, "va": va, "vg": vg})
def status():
    t = S["t"]; sc = S["scn"]
    hr = 72 + 3 * math.sin(t / 5); rr = 16 + math.sin(t / 7)
    cg = 0; vra = 12 + 2 * math.sin(t)
    if sc == "tachy": hr = 132; rr = 28; vra = 70; cg = 5
    if S.get("hrx", 0) > 0: hr = S["hrx"]
    m = {"t": "s", "sid": 1, "up": t * 1000, "cal": 2, "calp": 1, "cala": 1, "calmsg": "", "age": 35, "full": S["full"], "lic": lic(),
         "tb": 36.8 if sc == "normal" else 39.2, "chip": 52, "hot": 0, "rec": {"on": 0, "s": 0, "t": 0, "n": ""}, "mesh": S["mesh"], "mn": 2,
         "bat": {"ok": 1, "v": 3.9, "p": 70}, "temp": 31.0, "heap": 150000, "cli": 1, "ovf": 0, "i2c": 0, "au": 20, "aumax": 40,
         "hr": round(hr, 1), "hri": round(hr + 1, 1), "sqh": 80, "pres": 1, "rr": round(rr, 1), "rr1": round(rr, 1), "sqr": 75, "win": 300, "mot": S.get("mot", 0), "pf": 0, "ag": 6, "now": t * 1000, "pw": S.get("pw", 1), "mhz": 80, "boost": 0, "pol": 1, "apc": S.get("apc", 0)}
    if S["full"]:
        m.update({"nn": round(60000 / hr), "sdnn": 45.0, "rmssd": 38.0, "pnn50": 14.0, "hrmin": hr - 4, "hrmax": hr + 4, "b60": 70, "v60": 60, "cov": 0.9,
                  "sp": 6.0, "hra": hr, "hrs": hr, "hrb": hr, "rrs": rr, "amp": 1.0, "amp1": 1.0, "ampcv": 0.2, "pcv": 0.1, "ti": 1.5, "te": 2.0, "ie": 0.75,
                  "n5": 80, "ap5": 0, "apl": 0, "apc": S.get("apc", 0), "sig": 1, "mf": 0.0, "tbody": m["tb"], "vra": round(vra, 2), "vrg": 0.8, "vpk": 40.0, "vhf": 0.1,
                  "cgh": cg, "g": tiltg(t), "roll": 1, "pitch": round(2 + S.get("tilt", 0)), "tilt": S.get("tilt", 0), "vc": [1, 0, 0], "vg": [0, 1, 0], "vr": [0, 0, 1], "ab": [0, 0, 0]})
    return m
def verdict():
    if S["scn"] == "tachy":
        return {"t": "v", "sev": 2, "code": "tachy", "head": "Heart rate clearly above range", "cond": "Suspected tachycardia (132 bpm)", "score": 0.91, "mlc": 1, "mln": "tachycardia",
                "rel": 78, "agree": 1, "p": [0.03, 0.91, 0, 0.02, 0.03, 0, 0.01], "ev": [["a", "Heart rate 132 bpm is above 60-100."], ["i", "Model agrees"]], "miss": ["Body temperature is high."]}
    return {"t": "v", "sev": 0, "code": "ok", "head": "No abnormality found", "cond": "", "score": 0.97, "mlc": 0, "mln": "normal", "rel": 85, "agree": 1,
            "p": [0.97, 0.01, 0, 0.01, 0.01, 0, 0], "ev": [["i", "All values inside the age range."]], "miss": []}
def mesh():
    nd = [{"id": "A1B2C3", "name": "CardioA1B2", "hr": 72, "rr": 16, "sqh": 80, "sqr": 75, "sev": 0, "rel": 85, "mlc": 0, "mls": 0.97, "fl": 29, "age": 35, "t": 36.8, "rssi": 0, "hops": 0, "ago": 0, "up": 100, "self": 1},
          {"id": "D4E5F6", "name": "Bed 7", "hr": 131, "rr": 29, "sqh": 70, "sqr": 60, "sev": 3, "rel": 60, "mlc": 1, "mls": 0.88, "fl": 29, "age": 60, "t": 38.9, "rssi": -60, "hops": 1, "ago": 3, "up": 200, "self": 0},
          {"id": "010203", "name": "Bed 9", "hr": 66, "rr": 14, "sqh": 90, "sqr": 80, "sev": 0, "rel": 90, "mlc": 0, "mls": 0.95, "fl": 25, "age": 70, "t": None, "rssi": -72, "hops": 0, "ago": 50, "up": 300, "self": 0}]
    C = MCFG
    ok = C["grp"].lower() == "ward" and C["pin"] == "4821"          # the other monitors of the ward use group "ward", PIN 4821
    nd[0]["lbl"] = C["lbl"]; nd[1]["lbl"] = "ICU-2"; nd[2]["lbl"] = "ICU-4"
    near = [{"id": "0A0B0C", "name": "Bed 12", "lbl": "ICU-6", "st": 2, "rssi": -66, "ago": 2},
            {"id": "0D0E0F", "name": "Lab bench", "lbl": "", "st": 1, "rssi": -80, "ago": 4},
            {"id": "111213", "name": "Old unit", "lbl": "", "st": 3, "rssi": -75, "ago": 6}]
    if not ok:                                                   # wrong group or PIN: the ward monitors are not verified
        near = [dict(n, st=(1 if C["grp"].lower() != "ward" else 2), ago=n["ago"]) for n in nd[1:]] + near
        nd = nd[:1]
    return {"t": "m", "on": S["mesh"], "up": 1, "ttl": C["ttl"], "ver": 1, "grp": C["grp"], "open": 0 if C["pin"] else 1, "lbl": C["lbl"],
            "per": C["per"], "me": "A1B2C3", "ch": 6, "nodes": nd if S["mesh"] else [], "near": near if S["mesh"] else []}
MCFG = {"grp": "ward", "pin": "4821", "lbl": "Bed 4", "per": 2000, "ttl": 2}
def mcfg():
    C = MCFG
    return {"on": S["mesh"], "grp": C["grp"], "pinSet": 1 if C["pin"] else 0, "pinLen": len(C["pin"]), "lbl": C["lbl"], "per": C["per"], "ttl": C["ttl"], "id": "A1B2C3", "ch": 6}
CALLS = []
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _s(self, code, body, ct="application/json", extra=None):
        if not isinstance(body, bytes): body = json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); p = u.path; q = urllib.parse.parse_qs(u.query)
        if p in ("/", "/index.html"): return self._s(200, (ROOT / "web/dist.html").read_bytes(), "text/html; charset=utf-8")
        if p == "/api/license": return self._s(200, lic())
        if p == "/api/about": return self._s(200, {"name": "Test Designer", "email": "d@example.com", "phone": "+92 300 1234567", "wa": "923001234567", "org": "TestOrg", "fw": "1.2.0", "build": "x", "ml": "baseline", "dev": "CardioA1B2", "mac": "AA:BB:CC:DD:EE:FF", "flash": 4194304, "sketch": 1200000, "freesketch": 1500000, "cpu": 160, "ota": "open"})
        if p == "/api/profile": return self._s(200, PROF)
        if p == "/api/files": return self._s(200, {"ok": 1, "free": 500000, "total": 1000000, "rec": 0, "sec": 0, "tot": 0, "cur": "", "files": [{"n": k, "s": len(v)} for k, v in FILES.items()]})
        if p == "/api/file":
            n = q.get("n", [""])[0]
            return self._s(200, FILES[n], "application/octet-stream") if n in FILES else self._s(404, b"nf", "text/plain")
        if p == "/api/mesh": return self._s(200, mesh())
        if p == "/api/mesh/config": return self._s(200, mcfg())
        if p == "/api/status": return self._s(200, status())
        if p == "/api/wifi": return self._s(200, {"mode": "direct", "saved": "", "home": 0, "ip": "", "rssi": 0, "static": 0, "host": "cardio", "ap": 1, "apSsid": "CardioResp", "apIp": "192.168.4.1", "apCli": 1, "apOff": 0, "bootFailed": 0, "scanning": 0, "test": {"s": 0, "ssid": "", "ip": "", "msg": ""}})
        if p == "/api/log":
            L = json.loads((ROOT / "testdata/log.json").read_text()); return self._s(200, L)
        if p == "/api/power": return self._s(200, {"pw": S.get("pw", 1), "mhz": 80, "chip": 52, "hot": 0, "boost": 0})
        if p == "/api/set": S[q["k"][0]] = float(q["v"][0]); return self._s(200, {"ok": 1})
        if p == "/api/scn": S["scn"] = q["v"][0]; return self._s(200, {"ok": 1})
        self._s(404, {})
    def do_POST(self):
        u = urllib.parse.urlparse(self.path); p = u.path
        n = int(self.headers.get("Content-Length", 0) or 0); raw = self.rfile.read(n)
        form = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode("latin1") if "urlencoded" in self.headers.get("Content-Type", "") else "").items()}
        CALLS.append((p, form, len(raw)))
        if p == "/api/license":
            if form.get("token", "") == "good-token-" + "x" * 40: S.update(full=1, exp=0, lic_tok=1); return self._s(200, dict(lic(), ok=1, msg="Activated"))
            return self._s(400, dict(lic(), ok=0, msg="Not valid"))
        if p == "/api/license/remove": S.update(full=0, lic_tok=0); return self._s(200, lic())
        if p == "/api/profile":
            k = form.pop("kind", ""); PROF[k].update({a: b for a, b in form.items() if a in PROF[k]}); return self._s(200, PROF)
        if p == "/api/file/delete":
            if form.get("all") == "1": c = len(FILES); FILES.clear(); return self._s(200, {"ok": 1, "deleted": c})
            FILES.pop(form.get("n"), None); return self._s(200, {"ok": 1})
        if p == "/api/rec": return self._s(200, {"ok": 1, "name": "rec0003.crr", "sec": int(form.get("sec", 60))}) if form.get("op") == "start" else self._s(200, {"ok": 1})
        if p == "/api/mesh":
            if form.get("op") in ("on", "off"): S["mesh"] = 1 if form["op"] == "on" else 0
            elif form.get("op") in ("refresh", "verify") and not S["mesh"]: return self._s(409, {"ok": 0, "error": "Mesh is switched off"})
            return self._s(200, mesh())
        if p == "/api/mesh/config":
            import re as _re
            g = form.get("grp", MCFG["grp"]).strip()
            if not _re.fullmatch(r"[A-Za-z0-9 _-]{1,15}", g): return self._s(400, {"ok": 0, "error": "Group name: 1-15 letters, digits, spaces, - or _"})
            pin = form.get("pin", "")
            if form.get("pinclear") == "1": MCFG["pin"] = ""
            elif pin:
                if not _re.fullmatch(r"[0-9]{4,8}", pin): return self._s(400, {"ok": 0, "error": "PIN: 4 to 8 digits"})
                MCFG["pin"] = pin
            MCFG["grp"] = g; MCFG["lbl"] = form.get("lbl", MCFG["lbl"])[:15]
            MCFG["per"] = int(form.get("per", MCFG["per"])); MCFG["ttl"] = int(form.get("ttl", MCFG["ttl"]))
            if "on" in form: S["mesh"] = 1 if form["on"] == "1" else 0
            return self._s(200, mcfg())
        if p == "/api/temp": return self._s(200, {"ok": 1, "offset": 5.25}) if form.get("op") != "reset" else self._s(200, {"ok": 1, "offset": 0})
        if p == "/api/power": S["pw"] = int(form.get("pw", 1)); return self._s(200, {"pw": S["pw"], "mhz": 160 if S["pw"] == 0 else 80, "chip": 52, "hot": 0, "boost": 0})
        if p == "/api/time": return self._s(200, {"ok": 1})
        if p == "/api/update": return self._s(200, {"ok": 1, "bytes": n, "kind": "merged"})
        if p == "/api/calls": return self._s(200, [[a, b, c] for a, b, c in CALLS])
        self._s(404, {})
def http_thread():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("0.0.0.0", PORT), H) as s: s.serve_forever()
INFO = {"t": "i", "dev": "CardioA1B2", "host": "cardio", "fw": "1.2.0", "sensor": 1, "st": {"ran": 1, "pass": 1, "a": [1, 1, 1], "g": [1, 1, 1]},
        "calib": {"ok": 1, "att": 1, "gm": 1.0, "bias": [0, 0, 0], "g0": [0, 0, 1], "astd": [0, 0, 0], "gstd": [0, 0, 0], "drift": 0.1}, "age": 35, "group": "Adult (18-39 years)", "fsn": 200, "wsport": 81}
async def handler(ws):
    await ws.send(json.dumps(INFO))
    async def rd():
        async for m in ws: CALLS.append(("ws", m, 0))
    t = asyncio.create_task(rd())
    try:
        while True:
            S["t"] += 1
            await ws.send(json.dumps(status()))
            await ws.send(waves())
            if S["t"] % 4 == 1 and S["full"]: await ws.send(spectra())
            if S["full"]: await ws.send(json.dumps(verdict()))
            if S["t"] % 2 == 0: await ws.send(json.dumps(mesh()))
            await asyncio.sleep(0.5)
    except Exception: pass
    finally: t.cancel()
async def main():
    threading.Thread(target=http_thread, daemon=True).start()
    async with websockets.serve(handler, "0.0.0.0", 81, max_size=None):
        print("mock v1.2 http://localhost:%d" % PORT, flush=True); await asyncio.Future()
asyncio.run(main())
