# CardioResp v1.4.0 – fixes for issues.docx

Untouched original: `CardioResp_Monitor_v1.3_backup_before_fix/`

| Issue | Cause | Fix (file) |
|---|---|---|
| Page reload drops out / no response | The reloaded page left its old WebSocket open; the monitor only noticed after ~23 s, so its few connection slots filled up | Page closes its socket on reload/leave (`web/ui.js`); dead clients are dropped after ~8 s instead of 23 s (`CardioRespMonitor.ino`, `enableHeartbeat(5000,3000,1)`) |
| Very slow page | Every reload re-sent the whole 118 KB page (`no-store`), which blocked the loop | ETag + `no-cache`: a reload is answered with `304` in a few ms (`handleRoot`, `collectHeaders`) |
| Stuck / overheating | The full-page resends, plus the Wi-Fi radio never sleeping | The fixes above, plus `STA_MODEM_SLEEP 1` and `THERMAL_LIMIT_C 70` (both `config.h` and `config_yours.h`); the phone stops drawing traces that are hidden or unlicensed (`ui.js`) |
| Long page | – | Sub-tabs: **Live** = Monitor / Graphs only / Placement / Details; **Settings** = one tab per section. The choice is remembered (`index.html`, `ext.js`) |
| Graphs shown without a licence | – | Without a licence all traces, trend charts, analysis and the simple-view wave are hidden, with a message that points to Settings → Licence. The placement drawing still shows so the sensor can be set up (`index.html`, `ui.js`) |
| Simple mode, only live graphs | – | **Live → Graphs only** tab: shows just the heart and breathing traces, drawn taller |
| Compact, encrypted files | – | Recordings now hold waveforms as zig-zag varint deltas (about 2–3× smaller), and everything after the header is encrypted with a keystream. The page decrypts them; v1.3 files still open (`Recorder.cpp/.h`, `ext.js`) |
| Captive portal on Wi-Fi / simple names | Already built: `AP_ALWAYS_ON 1` keeps the self-opening network beside the home Wi-Fi, and the names are cardio, cardio2 … `.local` plus the DHCP host name | No change needed |

Note: the file encryption key (`REC_FILE_KEY` in `Recorder.h` = `REC_KEY` in `web/ext.js`) is also inside the page. That stops anyone from reading a file in flash or a downloaded `.crr` as plain data. It does not stop someone who has the dashboard. If you change the key, change it in both places.

## Rebuild
1. `py -3 -X utf8 tools_build_page.py` (already done; `webpage.h` is up to date)
2. Compile and flash with Arduino IDE / arduino-cli as before (copy `config_yours.h` over `config.h` for your build). The `.bin` files in `firmware/` are still v1.3 and must be rebuilt.
