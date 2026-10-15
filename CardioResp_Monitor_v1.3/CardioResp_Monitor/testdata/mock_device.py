#!/usr/bin/env python3
"""Mock CardioResp device: serves the dashboard and replays a recorded
engine stream (testdata/stream.jsonl) over a WebSocket on port 81.
   python3 mock_device.py [http_port] [speed] [start_s]"""
import asyncio, json, sys, threading, pathlib, http.server, socketserver
import websockets

ROOT = pathlib.Path(__file__).resolve().parent.parent
HTTP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
SPEED = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
START = float(sys.argv[3]) if len(sys.argv) > 3 else 200.0
STREAM = [json.loads(l) for l in open(ROOT / "testdata/stream.jsonl")]
LOG = (ROOT / "testdata/log.json").read_bytes()
INFO = {"t": "i", "dev": "CardioResp-A1B2C3", "host": "cardio-a1b2c3", "fw": "1.1.0", "sensor": 1,
        "st": {"ran": 1, "pass": 0, "a": [2.6, -1.5, 20.6], "g": [-8.7, -3.2, 2.1]},
        "calib": {"ok": 1, "att": 1, "gm": 1.2150, "bias": [-0.10, 0.71, -1.26], "g0": [0.038, -0.266, 0.963],
                  "astd": [0.0031, 0.0030, 0.0032], "gstd": [0.64, 0.11, 0.14], "drift": 0.06},
        "age": 35.0, "group": "Adult (18-39 years)", "fsn": 200, "wsport": 81}
CALMSG = ("Calibrated with a warning: the accelerometer reads 1.22 g instead of 1.00 g. The offset is corrected automatically; "
          "the 6-position calibration in the Device section removes it permanently.")
# ---- simulated network manager (mirrors NetManager JSON)
NET = {"mode": "direct", "saved": "", "home": 0, "ip": "", "gw": "", "mask": "", "dns": "", "rssi": 0, "static": 0,
       "host": "cardio-a1b2c3", "ap": 1, "apSsid": "CardioResp-A1B2C3", "apIp": "192.168.4.1", "apCli": 1, "apOff": 0,
       "bootFailed": 0, "scanning": 0, "test": {"s": 0, "ssid": "", "ip": "", "msg": ""}}
NETS = [["HomeNet", -52, 0], ["Clinic-Guest", -67, 1], ["Neighbour 2.4G", -81, 0]]
net_test = {"t0": 0, "pass": ""}
ACAL = {"t": "a", "n": 0, "cov": 0, "cap": 0, "msg": "Place the bare sensor still, one face up, then press Capture.", "corr": {"ok": 0}}
last_status = {"t": "s", "cal": 1, "calp": 0.4, "calmsg": "Calibrating - keep still", "age": 35}
commands = []
AGE = {"v": None}

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, body, ctype):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"):
            self._send(200, (ROOT / "web/dist.html").read_bytes(), "text/html; charset=utf-8")
        elif p == "/api/log":
            self._send(200, LOG, "application/json")
        elif p == "/api/status":
            self._send(200, json.dumps(last_status).encode(), "application/json")
        elif p == "/api/wifi":
            import time
            t = NET["test"]
            if t["s"] == 1 and time.time() - net_test["t0"] > 3:
                if net_test["pass"] == "correct123":
                    t.update({"s": 2, "ip": "192.168.1.57", "msg": "Connected"})
                    NET.update({"mode": "both", "saved": t["ssid"], "home": 1, "ip": "192.168.1.57", "gw": "192.168.1.1",
                                "mask": "255.255.255.0", "dns": "192.168.1.1", "rssi": -55, "apOff": 118})
                else:
                    t.update({"s": 3, "msg": "Wrong password, or the router refused the connection."})
            self._send(200, json.dumps(NET).encode(), "application/json")
        elif p == "/api/wifi/scan":
            self._send(200, json.dumps({"running": 0, "nets": NETS}).encode(), "application/json")
        elif p == "/api/commands":
            self._send(200, json.dumps(commands).encode(), "application/json")
        else:
            self.send_response(302); self.send_header("Location", "/"); self.end_headers()

    def do_POST(self):
        import urllib.parse, time
        n = int(self.headers.get("Content-Length", 0) or 0)
        form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(n).decode()).items()}
        commands.append({"POST": self.path, "form": {k: ("***" if k == "pass" else v) for k, v in form.items()}})
        if self.path == "/api/wifi/connect":
            if not form.get("ssid"):
                return self._send(400, b'{"error":"Network name must be 1-32 characters"}', "application/json")
            NET["test"] = {"s": 1, "ssid": form["ssid"], "ip": "", "msg": "Connecting..."}
            NET["host"] = form.get("host", NET["host"])
            net_test["t0"] = time.time(); net_test["pass"] = form.get("pass", "")
            return self._send(200, b'{"ok":1}', "application/json")
        if self.path == "/api/wifi/static":
            NET["static"] = 1
            return self._send(200, b'{"ok":1,"msg":"Address 192.168.1.57 is now fixed"}', "application/json")
        if self.path == "/api/wifi/forget":
            NET.update({"mode": "direct", "saved": "", "home": 0, "ip": "", "static": 0, "ap": 1})
            return self._send(200, b'{"ok":1}', "application/json")
        self._send(404, b'{}', "application/json")

def http_thread():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("0.0.0.0", HTTP_PORT), H) as s:
        s.serve_forever()

async def handler(ws):
    global last_status
    await ws.send(json.dumps(INFO))
    await ws.send(json.dumps(ACAL))
    # calibration phase for 1.5 s
    for k in range(3):
        st = {"t": "s", "sid": 1, "up": 1000 * k, "cal": 1, "calp": (k + 1) / 3, "cala": 1,
              "calmsg": "Calibrating - keep still and breathe normally", "age": 35,
              "bat": {"ok": 1, "v": 3.94, "p": 68}, "temp": 31.2}
        await ws.send(json.dumps(st)); await asyncio.sleep(0.5)
    async def reader():
        async for msg in ws:
            commands.append(msg)
            c = json.loads(msg)
            if c.get("cmd") == "acal":
                op = c.get("op")
                if op == "capture":
                    ACAL["cap"] = 1; ACAL["msg"] = "Capturing position %d - do not touch..." % (ACAL["n"] + 1)
                    await ws.send(json.dumps(ACAL)); await asyncio.sleep(2.0)
                    bit = 1 << ACAL["n"] if ACAL["n"] < 6 else 0
                    ACAL["n"] += 1; ACAL["cov"] |= bit; ACAL["cap"] = 0
                    ACAL["msg"] = "Position %d saved." % ACAL["n"]
                elif op == "save" and ACAL["n"] >= 4:
                    ACAL["corr"] = {"ok": 1, "off": [0.031, -0.042, 0.205], "gain": [1.021, 0.978, 1.046], "rms": 2.4, "full": 1}
                    ACAL["n"] = 0; ACAL["cov"] = 0; ACAL["msg"] = "Saved. Attach the sensor to the body and press Recalibrate."
                elif op == "clear":
                    ACAL["n"] = 0; ACAL["cov"] = 0; ACAL["msg"] = "Cleared."
                elif op == "remove":
                    ACAL["corr"] = {"ok": 0}; ACAL["msg"] = "Stored accelerometer calibration removed."
                await ws.send(json.dumps(ACAL))
            try:
                c = json.loads(msg)
                if c.get("cmd") == "age":          # behave like the firmware: apply + echo info
                    AGE["v"] = float(c["v"])
                    await ws.send(json.dumps(dict(INFO, age=AGE["v"])))
            except Exception:
                pass
    rt = asyncio.create_task(reader())
    t0 = asyncio.get_event_loop().time()
    idx = next(i for i, e in enumerate(STREAM) if e["ts"] >= START)
    base = STREAM[idx]["ts"]
    try:
        while idx < len(STREAM):
            e = STREAM[idx]
            due = t0 + (e["ts"] - base) / SPEED
            now = asyncio.get_event_loop().time()
            if due > now: await asyncio.sleep(due - now)
            m = e["m"]
            if m["t"] == "s":
                if AGE["v"] is not None: m = dict(m, age=AGE["v"])
                m = dict(m, calmsg=CALMSG)
                m["net"] = {"home": NET["home"], "ap": NET["ap"], "ssid": NET["saved"], "ip": NET["ip"], "rssi": NET["rssi"]}
                last_status = m
            await ws.send(json.dumps(m, separators=(",", ":")))
            idx += 1
        await asyncio.sleep(3600)
    finally:
        rt.cancel()

async def main():
    threading.Thread(target=http_thread, daemon=True).start()
    async with websockets.serve(handler, "0.0.0.0", 81, max_size=None):
        print(f"mock device: http://localhost:{HTTP_PORT}  ws :81  speed x{SPEED} from {START}s", flush=True)
        await asyncio.Future()

asyncio.run(main())
