# CardioResp Monitor v1.3 – test report

Date: 15 October 2026 · Firmware 1.3.0 · Arduino core esp32 3.3.12 · board ESP32-C3 (4 MB, `partitions.csv` unchanged from v1.2)

Everything below was run on a PC. The signal processing was driven by a physiological body model, and the dashboard by a
simulated monitor in a real browser. **No test was made on the physical controller with a real person.** Before relying on
the readings, compare them once against a pulse oximeter or ECG and a counted breathing rate, and agree the alarm limits
with the doctor. This is not a certified medical device.

## Summary

| Area | Test | Result |
|---|---|---|
| Firmware build, generic `config.h` | arduino-cli, warnings on | **OK**: 1 501 589 B = 84 % of the 1.69 MB slot, RAM 51 %, 0 warnings |
| Firmware build, your `config.h` | same | **OK**: 1 501 605 B = 84 %, RAM 51 %, 0 warnings |
| Flash images | bootloader, partitions and otadata compared with v1.2 `merged.bin` | **identical**, so an over-the-air update from v1.2 is possible |
| Web page in the firmware | gzip in `webpage.h` decoded and compared with `web/dist.html` | **identical** (372 KB, 116 KB gzip) |
| Licence key | searched in both images | only in your build, not in the generic one |
| Problems you reported (`host_test/sim_scen.cpp`) | 14 simulated people, 56 checks | **v1.3: 56 / 56** (the v1.2 engine passes 40 / 56) |
| Accuracy scenarios (`host_test/sim_test.cpp`) | 11 ten-minute scenarios + 12 sensor orientations | **11 / 11, 12 / 12**: HR error ≤ 0.14 bpm, RR error ≤ 0.44 /min |
| Illness patterns (`host_test/sim_abnormal.cpp`) | normal, fast or slow heart, fast breathing, arrest, cough, infection timing, pause | **13 / 13** |
| Controller vs page assessment (`testdata/xcheck_analytics.py`) | 10 470 verdicts and evidence texts | **10 470 / 10 470 identical** (3 484 of them with temperatures in °C and °F) |
| Alarm limits and doctor / nurse PIN (`host_test/alarm_settings_test.cpp`) | defaults per age, validation, PIN hash, lock-out | **all passed** |
| Simple names for several monitors (`host_test/name_test.cpp`) | cardio, cardio2 … cardio9, clashes, restarts | **all passed** |
| Mesh signature (`host_test/mesh_auth_test.cpp`) | 13 checks | **all passed** |
| Accelerometer calibration (`host_test/accelcal_test.cpp`) | full, partial, rejected | **passed** |
| Report and CSV maths (`testdata/test_core.js`) | 10-minute recording | **OK** |
| Dashboard in Chromium (`testdata/ui_test_v13.py`) | phone and desktop, light and dark | **148 / 148 checks passed**, no JavaScript errors |
| Recording report PDF | built in the browser from a recording, rendered to an image | temperature in °C and °F in the table, findings and chart |

## What the 148 browser checks cover

* **Ready, Start, measuring, Stop (40):** READY shows the placement and gravity (posture, movement, axis up), Start,
  8-s calibration, measuring, Stop with confirmation, stopped from another phone, started again elsewhere, no
  "measurement stopped" alarm after your own Stop.
* **Measurement problems and alarms (14):** fast breathing raises the breath count and not the heart rate (no heart-rate
  alarm), breathing pause warning at the age limit and urgent at 20 s with the siren, no-signal alarm.
* **Without a licence / without a sensor (11):** placement and gravity are still shown, and the page says what is missing.
* **Restart, factory reset, refused commands (12):** the page waits for the monitor and reconnects; the licence is kept
  unless the box is ticked; Start without a sensor and unknown commands are refused.
* **Simple full-screen view (16):** only heart rate and breathing pattern, alarms on top, landscape, Start button when idle.
* **Names, QR codes, own Wi-Fi, other monitors (14):** `cardio2.local` shown, both QR codes decoded with OpenCV
  (page address and Wi-Fi join), the "own network stays on" switch, and the list of monitors on the same Wi-Fi.
* **Alarm limits and PIN (29):** everybody can read them; editing needs the PIN; a wrong PIN is refused; impossible values are
  refused with a plain message; a saved limit is used by the alarm (heart alarm above 60 bpm); a second phone shows the
  same limits; back to the age defaults; PIN change; Lock; 5 wrong PINs lock the entry.
* **Body temperature in °C and °F (10):** limits shown in both units; the edit form has a °C / °F switch; a value typed
  in °F (100.0 °F) is stored as 37.78 °C on the monitor; the alarm list and the change note show both units; the
  thermometer calibration accepts °F; the header shows "body 36.9 °C / 98.4 °F".
* **General (2):** no sideways scrolling on a phone, no JavaScript errors during the whole run (dark theme and desktop
  layout were also captured as screenshots).

## Not covered by these tests

* Real hardware: the sensor on a real chest, the board's radio, its LED and BOOT button, and battery operation.
* Local names (`.local`) and the captive-portal pop-up on real phones and routers. These depend on the phone (Android
  needs Chrome or the IP address) and on the router (some block mDNS between devices).
* Long runs (hours) on the controller: memory over time, heat, and Wi-Fi reconnects.
* Clinical accuracy: the illness patterns are rules for drawing attention to a patient. They are not diagnoses.

## How to repeat

```
# PC tests (from the package folder)
g++ -O2 -std=c++17 -ICardioRespMonitor host_test/sim_scen.cpp CardioRespMonitor/VitalsEngine.cpp -o sim_scen && ./sim_scen
g++ -O2 -std=c++17 -ICardioRespMonitor host_test/sim_test.cpp CardioRespMonitor/VitalsEngine.cpp -o sim_test && ./sim_test
g++ -O2 -std=c++17 -ICardioRespMonitor -Ihost_test/stubs host_test/sim_abnormal.cpp CardioRespMonitor/VitalsEngine.cpp -o sim_abn && ./sim_abn
g++ -O2 -std=c++17 -Ihost_test/stubs -ICardioRespMonitor host_test/analytics_test.cpp -o analytics_test && python3 testdata/xcheck_analytics.py ./analytics_test
g++ -O2 -std=c++17 -DMESH_AUTH_OPENSSL -ICardioRespMonitor host_test/alarm_settings_test.cpp -lcrypto -o alarm_test && ./alarm_test
g++ -O2 -std=c++17 -ICardioRespMonitor host_test/name_test.cpp -o name_test && ./name_test
g++ -std=c++17 -DMESH_AUTH_OPENSSL -ICardioRespMonitor host_test/mesh_auth_test.cpp -lcrypto -o mesh_test && ./mesh_test
g++ -O2 -std=c++17 -ICardioRespMonitor host_test/accelcal_test.cpp -o accelcal_test && ./accelcal_test
node testdata/test_core.js
# dashboard (needs Python Playwright with Chromium, websockets, OpenCV for the QR check)
python3 testdata/mock_device_v13.py 8081 &
python3 testdata/ui_test_v13.py <folder for screenshots>
```
