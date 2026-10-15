/*
  =====================================================================
   CardioResp Monitor  -  heart-rate and breathing-pattern analyser
   ESP32-C3 Super Mini + MPU6050 (seismo- and gyro-cardiography)
  =====================================================================
   Board   : "ESP32C3 Dev Module"  (Arduino core esp32 by Espressif 3.x)
   Settings: USB CDC On Boot = Enabled, CPU 160 MHz, Flash 4MB
             (partitions.csv next to this file is used automatically)
   Library : "WebSockets" by Markus Sattler (Library Manager)

   Architecture (FreeRTOS tasks on the single RISC-V core):
     sensorTask   prio 5  MPU6050 FIFO @200 Hz -> VitalsEngine::pushSample
                          + start-up calibration (gravity + gyro offsets)
     analysisTask prio 2  VitalsEngine::analyze() once per second
     loopTask     prio 1  Wi-Fi AP, captive portal DNS, HTTP, WebSocket

   Phone: join the "CardioResp-XXXXXX" network (password on the Serial
   Monitor). The dashboard opens automatically (captive portal) or at
   http://192.168.4.1

   v1.2 additions (surgical, the v1.1 measurement code is unchanged):
     multi-page dashboard, licence (activation key), firmware update (sketch or
     merged .bin), ESP-NOW mesh, recording to flash, patient / nurse profiles,
     rule-based severity + tiny ML (on device and in page), chest vibration
     values with colour coding, sensor temperature in degC / degF.
   Partition table: partitions.csv in this folder (2 OTA slots + storage) -
   flash it once over USB; later updates work over Wi-Fi.

   NOTE: research / educational instrument - not a certified medical
   device. Results must be reviewed by a qualified clinician.
  =====================================================================
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <WebSocketsServer.h>
#include "mbedtls/sha256.h"

#include "config.h"
#ifndef POWER_PROFILE_DEFAULT
#define POWER_PROFILE_DEFAULT 1       // 0 Performance, 1 Cool, 2 Eco (older config.h files do not define it)
#endif
// ---- v1.3 options (defaults for older config.h files) ----
#ifndef WAIT_FOR_START
#define WAIT_FOR_START        1       // wait for "Start measurement" after power-up (resume by itself after a power cut)
#endif
#ifndef PIN_I2C_SDA_ALT
#define PIN_I2C_SDA_ALT       6       // second place to look for the sensor (frees GPIO8 LED and GPIO9 BOOT button)
#endif
#ifndef PIN_I2C_SCL_ALT
#define PIN_I2C_SCL_ALT       7
#endif
#ifndef STATUS_LED_AUTO
#define STATUS_LED_AUTO       1       // drive the on-board LED when its pin is not used by the sensor
#endif
#ifndef START_BUTTON_AUTO
#define START_BUTTON_AUTO     1       // BOOT button (GPIO9) = Start / Stop when it is not used by the sensor
#endif
#ifndef PIN_START_BUTTON
#define PIN_START_BUTTON      9
#endif
#include "MotionSensor.h"
#include "VitalsEngine.h"
#include "Protocol.h"
#include "AccelCal.h"
#include "NetManager.h"
#include "webpage.h"
#include "Analytics.h"
#include "License.h"
#include "ProfileStore.h"
#include "MeshNode.h"
#include "MeshAuth.h"
#include "Recorder.h"
#include "OtaUpdate.h"
#include "AlarmSettings.h"
#include <esp_wifi.h>
#include <time.h>

// ---------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------
static MPU6050Lite mpu;
static VitalsEngine engine;
static WebServer server(80);
static NetManager net;
static WebSocketsServer ws(WEBSOCKET_PORT);
static Preferences prefs;
static License lic;
static ProfileStore prof;
static MeshNode mesh;
static Recorder rec;
static OtaUpdate ota;
static AnaTracker g_anaT;
static Verdict g_verdict;
static bool g_verdictOk = false;
static float g_tempOffsetC = TEMP_OFFSET_C;
static uint32_t g_restartAt = 0;
static float g_chipTemp = NAN;
static bool g_hot = false;
static bool g_sntp = false;
static uint32_t g_anaSid = 0xFFFFFFFFu;
static uint8_t g_lastSev = 0;
static bool g_verdictPush = false;

static char g_devId[8], g_ssid[33], g_pass[24], g_host[24];

// CAL_READY (v1.3): sensor found, NOT measuring - the page shows placement and gravity and the Start button
enum CalState : uint8_t { CAL_IDLE = 0, CAL_RUNNING = 1, CAL_DONE = 2, CAL_NO_SENSOR = 3, CAL_READY = 4 };
static volatile CalState g_calState = CAL_IDLE;
// ---- v1.3: Start / Stop, placement preview, pins found, LED, button, temperature calibration flag
static volatile bool g_startRequest = false, g_stopRequest = false;
static bool g_runFlag = false;                  // a measurement is running (kept in flash: resume after a power cut)
static volatile bool g_runFlagDirty = false;
static int g_sdaPin = PIN_I2C_SDA, g_sclPin = PIN_I2C_SCL;
static int g_ledPin = -1, g_btnPin = -1;
static bool g_tcal = false;                     // body temperature calibrated against a thermometer
static volatile float g_pvG[3] = {0, 0, 1};     // preview gravity (unit vector) while not measuring
static volatile float g_pvMot = 0;              // preview: angular-rate RMS (deg/s)
static volatile bool g_pvValid = false;
static volatile uint32_t g_pvUpdMs = 0;
static volatile bool g_urgent = false;
static AlarmCfg g_alm;                          // v1.3: alarm limits set by the doctor / nurse (AlarmSettings.h)
static float alarmApneaLimit();          // urgent alarm (breathing pause / heart lost): LED flashes fast
static bool probeSensorOn(int sda, int scl);
static volatile float g_calProgress = 0;
static volatile int g_calAttempt = 0;
static char g_calMsg[200] = "Starting";
static volatile bool g_recalRequest = true;     // calibrate at every start-up
static volatile bool g_resetRequest = false;
static volatile bool g_calSaveRequest = false;
static volatile bool g_infoDirty = false;
static bool g_sensorOk = false;
static MpuSelfTest g_selfTest;
static CalibData g_cal, g_prevCal;
static bool g_havePrevCal = false;
static float g_ageYears = DEFAULT_AGE_YEARS;

static volatile float g_tempC = NAN;
static volatile uint32_t g_fifoOverflows = 0, g_analyzeUs = 0, g_analyzeMaxUs = 0;
static volatile float g_fsMeasured = VE_FS;
static float g_battV = NAN;
static int g_battPct = -1;
static bool g_battPresent = false;
static uint32_t g_lastClientSeenMs = 0;
static SemaphoreHandle_t g_engineMtx = nullptr;   // analysis <-> (re)start
// one-time accelerometer accuracy calibration (6 positions)
static AccelCorrection g_accCorr, g_accCorrNew;
static volatile bool g_accCorrUpdate = false, g_acalDirty = false;
static AccelCalSession g_acal;

static char g_json[6144];

// ---------------------------------------------------------------------
//  Unique credentials per board
// ---------------------------------------------------------------------
static void makeCredentials() {
  uint64_t chip = ESP.getEfuseMac();
  uint8_t mac[6];
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(chip >> (8 * i));
  snprintf(g_devId, sizeof(g_devId), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  snprintf(g_ssid, sizeof(g_ssid), "%s%s", AP_SSID_PREFIX, g_devId);
  snprintf(g_host, sizeof(g_host), "cardio-%s", g_devId);
  for (char* p = g_host; *p; p++) *p = tolower(*p);
  if (strlen(AP_FIXED_PASSWORD) >= 8) {
    strlcpy(g_pass, AP_FIXED_PASSWORD, sizeof(g_pass));
    return;
  }
  uint8_t in[6 + 64];
  size_t saltLen = strlen(AP_PASSWORD_SALT);
  if (saltLen > 64) saltLen = 64;
  memcpy(in, mac, 6);
  memcpy(in + 6, AP_PASSWORD_SALT, saltLen);
  uint8_t h[32];
  mbedtls_sha256(in, 6 + saltLen, h, 0);
  static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";  // no 0/O/1/l/I
  const size_t na = sizeof(alpha) - 1;
  for (int i = 0; i < 10; i++) g_pass[i] = alpha[h[i] % na];
  g_pass[10] = 0;
}

// ---------------------------------------------------------------------
//  Status LED
// ---------------------------------------------------------------------
// v1.3: the LED pin is decided at start-up: the configured LED, or the on-board LED (GPIO8) when the
// sensor was found on the alternative pins and GPIO8 is therefore free.
static inline void ledWrite(bool on) {
  if (g_ledPin < 0) return;
  digitalWrite(g_ledPin, (on ^ (STATUS_LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
}

// ---------------------------------------------------------------------
//  Battery (optional divider on an ADC pin)
// ---------------------------------------------------------------------
static int liionPercent(float v) {
  static const float V[] = {3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f, 3.82f, 3.84f,
                            3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f, 4.11f, 4.15f, 4.20f};
  if (v <= V[0]) return 0;
  if (v >= V[20]) return 100;
  for (int i = 1; i <= 20; i++) {
    if (v < V[i]) {
      float f = (v - V[i - 1]) / (V[i] - V[i - 1]);
      return (int)lroundf(5.0f * (i - 1) + 5.0f * f);
    }
  }
  return 100;
}

static void updateBattery() {
#if BATTERY_ENABLED
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_BATTERY_ADC);
  float v = (mv / 16.0f) * BATTERY_DIVIDER_RATIO * BATTERY_CAL_FACTOR / 1000.0f;
  if (v < 2.8f || v > 4.6f) { g_battPresent = false; g_battV = NAN; g_battPct = -1; return; }
  g_battV = (!g_battPresent || isnan(g_battV)) ? v : 0.85f * g_battV + 0.15f * v;
  g_battPresent = true;
  g_battPct = liionPercent(g_battV);
#endif
}

// ---------------------------------------------------------------------
//  Sensor task: calibration + continuous acquisition
// ---------------------------------------------------------------------
static int16_t s_raw[64 * 6];

static inline void toPhys(const int16_t* r, float v[6]) {
  for (int k = 0; k < 3; k++) { v[k] = r[k] / MPU_ACC_LSB_PER_G; v[3 + k] = r[3 + k] / MPU_GYRO_LSB_PER_DPS; }
}

// v1.3: placement preview while NOT measuring (before Start, during calibration, after Stop):
// gravity direction from the low-passed accelerometer and how still the sensor is (angular-rate spread).
static void previewSample(const float v[6]) {
  static float a[3] = {0, 0, 1}, wm[3] = {0, 0, 0}, w2 = 0;
  static bool init = false;
  static int cnt = 0;
  const float ka = 1.0f / (0.25f * VE_FS), kb = 1.0f / (5.0f * VE_FS), kw = 1.0f / (0.4f * VE_FS);
  if (!init) { for (int i = 0; i < 3; i++) { a[i] = v[i]; wm[i] = v[3 + i]; } init = true; }
  float d2 = 0;
  for (int i = 0; i < 3; i++) { a[i] += ka * (v[i] - a[i]); const float d = v[3 + i] - wm[i]; d2 += d * d; }
  w2 += kw * (d2 - w2);
  if (w2 < 9.0f) for (int i = 0; i < 3; i++) wm[i] += kb * (v[3 + i] - wm[i]);    // offset follows only while still
  if (++cnt >= 10) {
    cnt = 0;
    const float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n > 0.3f) for (int i = 0; i < 3; i++) g_pvG[i] = a[i] / n;
    g_pvMot = sqrtf(w2 > 0 ? w2 : 0);
    g_pvValid = true; g_pvUpdMs = millis();
  }
}

static void runCalibration() {
  xSemaphoreTake(g_engineMtx, portMAX_DELAY);
  engine.halt();
  xSemaphoreGive(g_engineMtx);
  g_calState = CAL_RUNNING;
  g_calProgress = 0;
  g_infoDirty = true;
  const int settle = VE_FS / 2;
  const int need = CALIBRATION_SECONDS * VE_FS;
  CalibAccumulator acc;
  CalibData cal;
  char msg[96] = "";
  bool ok = false;
  for (int attempt = 1; attempt <= CALIBRATION_ATTEMPTS && !ok; attempt++) {
    g_calAttempt = attempt;
    if (attempt == 1) snprintf(g_calMsg, sizeof(g_calMsg), "Calibrating - keep still and breathe normally");
    mpu.resetFifo();
    acc.reset();
    int got = 0, skipped = 0;
    uint32_t t0 = millis();
    while (got < need && millis() - t0 < (uint32_t)(CALIBRATION_SECONDS + 6) * 1000u) {
      int n = mpu.readFifo(s_raw, 64);
      if (n == -2 || mpu.fifoOverflow()) { mpu.resetFifo(); n = 0; }
      for (int i = 0; i < n && got < need; i++) {
        const int16_t* r = s_raw + i * 6;
        if (skipped < settle) { skipped++; continue; }
        float v[6];
        toPhys(r, v);
        g_accCorr.apply(v[0], v[1], v[2]);
        acc.add(v[0], v[1], v[2], v[3], v[4], v[5]);
        previewSample(v);                   // v1.3: the placement bubble keeps moving during calibration
        got++;
      }
      g_calProgress = (float)got / need;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    ok = acc.finish(cal, msg, sizeof(msg));
    cal.attempts = attempt;
    if (!ok) {
      snprintf(g_calMsg, sizeof(g_calMsg), "Attempt %d/%d: %s", attempt, CALIBRATION_ATTEMPTS, msg);
      Serial.printf("[CAL] %s\n", g_calMsg);
      if (attempt < CALIBRATION_ATTEMPTS) vTaskDelay(pdMS_TO_TICKS(1500));
    }
  }
  if (ok && cal.gWarn)
    snprintf(g_calMsg, sizeof(g_calMsg), "Calibrated with a warning: the accelerometer reads %.2f g instead of 1.00 g. The offset is corrected automatically; "
             "the 6-position calibration in the Device section removes it permanently.", cal.gMag);
  else if (ok) snprintf(g_calMsg, sizeof(g_calMsg), "Calibrated: |g| = %.3f g, tilt reference stored", cal.gMag);
  else snprintf(g_calMsg, sizeof(g_calMsg), "Calibrated with a warning: %s", msg);
  g_cal = cal;
  xSemaphoreTake(g_engineMtx, portMAX_DELAY);
  engine.begin(cal, g_ageYears);
  xSemaphoreGive(g_engineMtx);
  mpu.resetFifo();
  g_calProgress = 1;
  g_calState = CAL_DONE;
  g_calSaveRequest = true;
  g_infoDirty = true;
  Serial.printf("[CAL] %s  gyro bias = %.2f %.2f %.2f dps  g0 = %.3f %.3f %.3f\n", g_calMsg,
                cal.gyroBias[0], cal.gyroBias[1], cal.gyroBias[2], cal.g0[0], cal.g0[1], cal.g0[2]);
}

static void sensorTask(void*) {
  uint32_t lastReadUs = micros();
  uint32_t fsT0 = millis(), fsCount = 0;
  bool fsWindowClean = true;
  uint32_t lastTempMs = 0;
  for (;;) {
    if (!g_sensorOk) {
      // sensor missing (loose wire?): retry every 3 s and start once it appears
      vTaskDelay(pdMS_TO_TICKS(3000));
      bool found = mpu.begin(Wire, 0x68) || mpu.begin(Wire, 0x69);
      if (!found && g_ledPin < 0 && g_btnPin < 0 && PIN_I2C_SDA_ALT >= 0 && PIN_I2C_SCL_ALT >= 0) {
        // v1.3: not found yet: also look on the other pair of pins (the LED / button are not in use, so this is safe)
        const bool onAlt = g_sdaPin == PIN_I2C_SDA_ALT;
        const int sda = onAlt ? PIN_I2C_SDA : PIN_I2C_SDA_ALT, scl = onAlt ? PIN_I2C_SCL : PIN_I2C_SCL_ALT;
        found = probeSensorOn(sda, scl);
        if (found) { g_sdaPin = sda; g_sclPin = scl; } else probeSensorOn(g_sdaPin, g_sclPin);
      }
      if (found) {
        Serial.printf("[MPU] %s found at 0x%02X (SDA GPIO%d / SCL GPIO%d)\n", mpu.model(), mpu.address(), g_sdaPin, g_sclPin);
        g_sensorOk = true;
        // v1.3: a measurement that was running continues by itself; otherwise wait for Start
        if (g_runFlag || !WAIT_FOR_START) { g_calState = CAL_IDLE; g_recalRequest = true; }
        else { g_calState = CAL_READY; snprintf(g_calMsg, sizeof(g_calMsg), "Sensor ready - place it on the chest and press Start"); }
        mpu.resetFifo();
        g_infoDirty = true;
      }
      continue;
    }
    if (g_accCorrUpdate) { g_accCorr = g_accCorrNew; g_accCorrUpdate = false; }
    // ---- v1.3: Start / Stop
    if (g_stopRequest) {
      g_stopRequest = false; g_startRequest = false; g_recalRequest = false;
      xSemaphoreTake(g_engineMtx, portMAX_DELAY);
      engine.halt();
      xSemaphoreGive(g_engineMtx);
      g_calState = CAL_READY;
      snprintf(g_calMsg, sizeof(g_calMsg), "Measurement stopped - press Start to measure again");
      g_runFlag = false; g_runFlagDirty = true;
      mpu.resetFifo();
      g_infoDirty = true;
      Serial.println(F("[RUN] measurement stopped"));
    }
    if (g_startRequest) {
      g_startRequest = false;
      g_recalRequest = true;                // calibrate (keep still 8 s), then measure
      g_runFlag = true; g_runFlagDirty = true;
      Serial.println(F("[RUN] measurement started"));
    }
    if (g_recalRequest) {
      g_recalRequest = false;
      runCalibration();
      lastReadUs = micros(); fsT0 = millis(); fsCount = 0; fsWindowClean = true;
      continue;
    }
    if (g_resetRequest) {
      g_resetRequest = false;
      xSemaphoreTake(g_engineMtx, portMAX_DELAY);
      engine.begin(g_cal, g_ageYears);
      xSemaphoreGive(g_engineMtx);
      mpu.resetFifo();
      g_infoDirty = true;
    }
    bool ovf = mpu.fifoOverflow();
    int n = ovf ? -2 : mpu.readFifo(s_raw, 64);
    uint32_t nowUs = micros();
    // sensor unplugged / loose wire: I2C reads keep failing -> stop, report and re-detect
    static int s_i2cFailRun = 0;
    s_i2cFailRun = (n == -1) ? s_i2cFailRun + 1 : 0;
    if (s_i2cFailRun > 60) {
      s_i2cFailRun = 0;
      xSemaphoreTake(g_engineMtx, portMAX_DELAY);
      engine.halt();
      xSemaphoreGive(g_engineMtx);
      g_sensorOk = false;
      g_calState = CAL_NO_SENSOR;
      snprintf(g_calMsg, sizeof(g_calMsg), "Sensor connection lost - check the sensor cable; measurement restarts automatically");
      Serial.printf("[MPU] %s (SDA GPIO%d / SCL GPIO%d)\n", g_calMsg, PIN_I2C_SDA, PIN_I2C_SCL);
      g_infoDirty = true;
      continue;
    }
    if (n < 0) {
      // FIFO overflow / misalignment: estimate lost samples, keep the time base
      mpu.resetFifo();
      g_fifoOverflows = g_fifoOverflows + 1;
      uint32_t lost = (uint32_t)((nowUs - lastReadUs) * (g_fsMeasured / 1e6f));
      engine.pushGap((int)(lost > 400 ? 400 : lost));
      lastReadUs = micros();
      fsWindowClean = false;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    for (int i = 0; i < n; i++) {
      const int16_t* r = s_raw + i * 6;
      float v[6];
      toPhys(r, v);
      if (g_acal.capturing && g_acal.addSample(v)) g_acalDirty = true;   // raw data for the 6-position calibration
      g_accCorr.apply(v[0], v[1], v[2]);
      if (engine.isRunning()) engine.pushSample(v[0], v[1], v[2], v[3], v[4], v[5]);
      else previewSample(v);                                              // v1.3: not measuring: placement preview only
    }
    if (n > 0) lastReadUs = nowUs;
    fsCount += n;
    // true sample-rate measurement against the ESP32 crystal (MPU clock is +-1..2 %)
    uint32_t el = millis() - fsT0;
    if (el >= 30000) {
      if (fsWindowClean) {
        float fs = fsCount * 1000.0f / el;
        if (fs > 150 && fs < 250) { g_fsMeasured = fs; engine.setFsActual(fs); }
      }
      fsT0 = millis(); fsCount = 0; fsWindowClean = true;
    }
    if (millis() - lastTempMs > 2000) {
      lastTempMs = millis();
      float t;
      if (mpu.readTemperature(t)) g_tempC = t;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void analysisTask(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
    if (g_calState != CAL_DONE) continue;
    xSemaphoreTake(g_engineMtx, portMAX_DELAY);
    if (engine.isRunning()) {
      uint32_t t0 = micros();
      engine.analyze();
      uint32_t dt = micros() - t0;
      g_analyzeUs = dt;
      if (dt > g_analyzeMaxUs) g_analyzeMaxUs = dt;
    }
    xSemaphoreGive(g_engineMtx);
  }
}

// ---------------------------------------------------------------------
//  v1.2 housekeeping (loop task, once per second)
// ---------------------------------------------------------------------
static inline float bodyTempC() { float t = g_tempC; return isnan(t) ? NAN : anaR(t + g_tempOffsetC, 1); }

static void meshPublish(const VitalsResults* r) {
  MeshPkt p; memset(&p, 0, sizeof p);
  const char* nm = prof.patient.name[0] ? prof.patient.name : g_devId;
  strlcpy(p.name, nm, sizeof p.name);
  strlcpy(p.label, mesh.cfg().lbl[0] ? mesh.cfg().lbl : prof.patient.bed, sizeof p.label);
  p.t100 = INT16_MIN;
  if (!isnan(bodyTempC())) p.t100 = (int16_t)lroundf(bodyTempC() * 100.0f);
  p.ageY = (uint8_t)(g_ageYears > 255 ? 255 : g_ageYears);
  p.upS = millis() / 1000;
  p.flags = (g_sensorOk ? MESH_F_SENSOR : 0) | (lic.full() ? MESH_F_FULL : 0) | (g_calState == CAL_DONE ? MESH_F_CAL : 0);
  if (r) {
    if (r->heartPresent && !isnan(r->hr1m)) p.hr10 = (uint16_t)dspClamp(r->hr1m * 10.0f, 0, 65535);
    if (!isnan(r->rr5m)) p.rr10 = (uint16_t)dspClamp(r->rr5m * 10.0f, 0, 65535);
    p.sqh = r->sqiH; p.sqr = r->sqiR;
    if (r->heartPresent) p.flags |= MESH_F_PRES;
    if (r->motion) p.flags |= MESH_F_MOTION;
    if (g_verdictOk) { p.sev = g_verdict.severity; p.rel = g_verdict.reliability; p.mlc = g_verdict.mlClass; p.mls = (uint8_t)lroundf(g_verdict.mlScore * 100.0f); }
    else {                                   // no licence: plain range check only
      const AgeProfile& P = ageProfileFor(g_ageYears);
      int s = 0;
      if (!isnan(r->hr1m) && (r->hr1m > P.hrHi || r->hr1m < P.hrLo)) s = 1;
      if (!isnan(r->rr5m) && (r->rr5m > P.rrHi || r->rr5m < P.rrLo)) s = 1;
      p.sev = s;
    }
  }
  mesh.setSelf(p, g_devId);
}

static void secondTick(uint32_t now) {
  lic.tick();
  if (!g_sntp && net.homeUp()) { configTime(0, 0, "pool.ntp.org", "time.google.com"); g_sntp = true; }
  if (g_sntp && net.homeUp()) { time_t t = time(nullptr); if (t > 1735689600) lic.offerTime((uint32_t)t, true); }
  const bool running = g_calState == CAL_DONE && engine.isRunning();
  VitalsResults r;
  if (running) engine.getResults(r);
  if (running) {
    if (engine.session() != g_anaSid) { g_anaSid = engine.session(); g_anaT.reset(); }
    const AgeProfile& P = ageProfileFor(g_ageYears);
    AnaIn in = anaFromResults(r, bodyTempC(), g_tcal);
    g_anaT.update(in, P, r.nowMs);
    if (lic.full()) {
      anaEvaluate(g_anaT, in, P, r.nowMs, g_verdict);
      g_verdictOk = true;
      if (g_verdict.severity != g_lastSev) { g_lastSev = g_verdict.severity; g_verdictPush = true; }
    } else g_verdictOk = false;
    // v1.3: urgent = breathing pause beyond the age limit, or a critical verdict (heart signal lost, very slow heart)
    g_urgent = (alarmApneaLimit() > 0 && r.currentPause >= alarmApneaLimit()) || (g_verdictOk && g_verdict.severity >= 3);
    if (rec.active()) {
      RecRow row; memset(&row, 0, sizeof row);
      row.hr10 = (r.heartPresent && !isnan(r.hr1m)) ? (uint16_t)dspClamp(r.hr1m * 10.0f, 0, 65535) : 0;
      row.rr10 = isnan(r.rr5m) ? 0 : (uint16_t)dspClamp(r.rr5m * 10.0f, 0, 65535);
      row.sqh = r.sqiH; row.sqr = r.sqiR; row.sev = g_verdictOk ? g_verdict.severity : 0;
      row.flags = (r.heartPresent ? 1 : 0) | (r.motion ? 2 : 0) | (g_verdictOk ? 4 : 0);
      row.tc100 = isnan(bodyTempC()) ? INT16_MIN : (int16_t)lroundf(bodyTempC() * 100.0f);
      row.rmssd10 = isnan(r.rmssd) ? 0 : (uint16_t)dspClamp(r.rmssd * 10.0f, 0, 65535);
      rec.tick(engine, row);
    }
  } else {
    g_verdictOk = false;
    g_urgent = false;
    if (rec.active()) rec.stop(false);
  }
  if (mesh.enabled()) meshPublish(running ? &r : nullptr);
}

// ---- power profiles (heat) -------------------------------------------
struct PwrCfg { uint8_t mhz; uint16_t wave, status, spec; uint8_t loopMs; wifi_power_t tx; };
static const PwrCfg PWR[3] = {
  {160, 100, 1000, 2000, 2, WIFI_POWER_8_5dBm},     // Performance
  { 80, 200, 1000, 3000, 4, WIFI_POWER_5dBm},       // Cool
  { 80, 400, 2000, 6000, 6, WIFI_POWER_2dBm},       // Eco
};
static uint8_t g_pwr = POWER_PROFILE_DEFAULT;
static uint32_t g_boostUntil = 0, g_lastOvf = 0;
static bool g_idleEco = false;

static void applyPower() {
  const PwrCfg& c = PWR[g_pwr > 2 ? 1 : g_pwr];
  uint8_t mhz = c.mhz;
  if (g_boostUntil && (int32_t)(millis() - g_boostUntil) < 0) mhz = 160;
  else g_boostUntil = 0;
  if (g_hot && g_boostUntil == 0) mhz = 80;                  // too hot: stay at the low clock
  if ((int)ESP.getCpuFreqMHz() != mhz) { setCpuFrequencyMhz(mhz); Serial.printf("[PWR] CPU %d MHz\n", (int)mhz); }
  if (WiFi.getMode() != WIFI_OFF) {
    wifi_power_t want = c.tx;
    if (g_hot && want > WIFI_POWER_5dBm) want = WIFI_POWER_5dBm;
    if (WiFi.getTxPower() != want) WiFi.setTxPower(want);
  }
}

static void thermalTick() {
  g_chipTemp = temperatureRead();
  if (!isnan(g_chipTemp)) {
    if (!g_hot && g_chipTemp > THERMAL_LIMIT_C) { g_hot = true; Serial.printf("[HEAT] chip %.0f C: lowering stream rate\n", g_chipTemp); }
    else if (g_hot && g_chipTemp < THERMAL_LIMIT_C - 6.0f) { g_hot = false; Serial.println(F("[HEAT] cooled down: normal stream rate")); }
  }
  // low-clock profiles: go to 160 MHz for 5 minutes if the analysis is slow or samples are being lost
  if (PWR[g_pwr > 2 ? 1 : g_pwr].mhz < 160 && !g_hot) {
    const bool slow = g_analyzeUs > 650000UL, lost = g_fifoOverflows != g_lastOvf;
    if (slow || lost) { g_boostUntil = millis() + 300000UL; Serial.printf("[PWR] %s: boosting CPU to 160 MHz for 5 min\n", slow ? "analysis slow" : "samples lost"); }
  }
  g_lastOvf = g_fifoOverflows;
  applyPower();
}

// ---------------------------------------------------------------------
//  WebSocket streaming
// ---------------------------------------------------------------------
static StreamCursor s_cursor;

static void sendWave() {
  if (g_calState != CAL_DONE || !engine.isRunning()) return;
  VitalsResults r;
  engine.getResults(r);
  JBuf j(g_json, sizeof(g_json));
  if (protoWave(j, engine, s_cursor, r.heartPresent)) ws.broadcastTXT(g_json, j.len);
}

// v1.3: orientation for the placement card - from the engine while measuring, from the preview otherwise
static void fillOrient(OrientInfo& o, const VitalsResults* r) {
  if (r) {
    for (int i = 0; i < 3; i++) o.g[i] = r->g[i];
    o.roll = r->roll; o.pitch = r->pitch; o.tilt = r->tiltFromCal; o.mot = r->motion; o.post = r->postureChanged;
    o.mf = r->motionFrac60; o.src = 1;
    o.still = r->motion ? 0 : (int)dspClamp(100.0f * (1.0f - r->motionFrac60), 0, 100);
    return;
  }
  for (int i = 0; i < 3; i++) o.g[i] = g_pvG[i];
  o.roll = atan2f(o.g[1], o.g[2]) * DSP_RAD2DEG;
  o.pitch = atan2f(-o.g[0], sqrtf(o.g[1] * o.g[1] + o.g[2] * o.g[2])) * DSP_RAD2DEG;
  const bool haveCal = g_cal.attempts > 0 || g_havePrevCal;
  const CalibData& c = g_cal.attempts > 0 ? g_cal : g_prevCal;
  o.tilt = haveCal ? acosf(dspClamp(o.g[0] * c.g0[0] + o.g[1] * c.g0[1] + o.g[2] * c.g0[2], -1.0f, 1.0f)) * DSP_RAD2DEG : NAN;
  const float m = g_pvMot;
  o.mot = m > 6.0f; o.post = false; o.mf = NAN; o.src = 2;
  o.still = g_pvValid ? (int)dspClamp(100.0f - 12.0f * m, 0, 100) : -1;
}

static void sendStatus() {
  JBuf j(g_json, sizeof(g_json));
  j.raw("{\"t\":\"s\"");
  j.ki("sid", engine.session());
  j.ki("up", millis());
  j.ki("cal", g_calState); j.kf("calp", g_calProgress, 2); j.ki("cala", g_calAttempt); j.ks("calmsg", g_calMsg);
  j.kf("age", g_ageYears, 2);
  j.ki("run", g_runFlag ? 1 : 0); j.ki("wfs", WAIT_FOR_START); j.ki("led", g_ledPin >= 0 ? 1 : 0); j.ki("btn", g_btnPin >= 0 ? 1 : 0);
  j.ki("tcal", g_tcal ? 1 : 0);
  j.ki("alv", (long)g_alm.seq);                         // v1.3: alarm limits version (pages reload them when it changes)
  OrientInfo o;
  if (g_calState == CAL_DONE && engine.isRunning()) {
    VitalsResults r;
    engine.getResults(r);
    if (lic.full()) { protoResults(j, r); j.kf("tbody", bodyTempC(), 1); fillOrient(o, &r); j.ki("still", o.still); j.ki("osrc", o.src); }
    else { protoResultsLite(j, r); fillOrient(o, &r); protoOrientation(j, o); }
  } else if (g_sensorOk && g_pvValid) {
    fillOrient(o, nullptr); protoOrientation(j, o);     // before Start / while calibrating / after Stop
  }
  j.ki("full", lic.full() ? 1 : 0);
  j.key("lic"); j.raw("{"); lic.writeJson(j); j.raw("}");
  j.kf("tb", bodyTempC(), 1); j.kf("chip", g_chipTemp, 0); j.ki("hot", g_hot ? 1 : 0); j.ki("pw", g_pwr); j.ki("mhz", (long)ESP.getCpuFreqMHz()); j.ki("boost", g_boostUntil ? 1 : 0);
  j.key("rec"); j.raw("{"); j.ki("on", rec.active() ? 1 : 0); j.ki("s", rec.elapsed()); j.ki("t", rec.total()); j.ks("n", rec.currentName()); j.raw("}");
  j.ki("mesh", mesh.enabled() ? 1 : 0); j.ki("mn", mesh.count());
  j.key("bat"); j.raw("{"); j.ki("ok", g_battPresent ? 1 : 0); j.kf("v", g_battV, 2); j.ki("p", g_battPct); j.raw("}");
  j.kf("temp", g_tempC, 1);
  j.ki("heap", ESP.getFreeHeap());
  j.ki("minheap", ESP.getMinFreeHeap());
  j.ki("cli", ws.connectedClients());
  net.writeShort(j);
  j.ki("ovf", g_fifoOverflows);
  j.ki("i2c", mpu.i2cErrors());
  j.ki("au", g_analyzeUs / 1000); j.ki("aumax", g_analyzeMaxUs / 1000);
  j.raw("}");
  ws.broadcastTXT(g_json, j.len);
}

static void sendSpectra() {
  if (g_calState != CAL_DONE || !engine.isRunning()) return;
  static VitalsSpectra sp;
  engine.getSpectra(sp);
  JBuf j(g_json, sizeof(g_json));
  protoSpectra(j, sp);
  ws.broadcastTXT(g_json, j.len);
}

static void writeInfo(JBuf& j) {
  j.raw("{\"t\":\"i\"");
  j.ks("dev", g_ssid); j.ks("host", net.host()); j.ks("fw", FW_VERSION);
  j.ki("sensor", g_sensorOk ? 1 : 0);
  j.key("st"); j.raw("{"); j.ki("ran", g_selfTest.ran); j.ki("pass", g_selfTest.passed);
  j.kv3("a", g_selfTest.accDev, 1); j.kv3("g", g_selfTest.gyroDev, 1); j.raw("}");
  j.key("calib"); j.raw("{");
  j.ki("ok", g_cal.stillOk ? 1 : 0); j.ki("att", g_cal.attempts); j.kf("gm", g_cal.gMag, 4);
  j.kv3("bias", g_cal.gyroBias, 3); j.kv3("g0", g_cal.g0, 3); j.kv3("astd", g_cal.accStd, 4); j.kv3("gstd", g_cal.gyroStd, 3);
  float drift = NAN;
  if (g_havePrevCal) {
    float d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (g_cal.gyroBias[i] - g_prevCal.gyroBias[i]) * (g_cal.gyroBias[i] - g_prevCal.gyroBias[i]);
    drift = sqrtf(d2);
  }
  j.kf("drift", drift, 2);
  j.raw("}");
  j.kf("age", g_ageYears, 2);
  const AgeProfile& P = ageProfileFor(g_ageYears);
  j.ks("group", P.name);
  j.ki("fsn", VE_FS);
  j.ki("wsport", WEBSOCKET_PORT);
  j.ki("sda", g_sdaPin); j.ki("scl", g_sclPin); j.ki("ledpin", g_ledPin); j.ki("btnpin", g_btnPin); j.ki("wfs", WAIT_FOR_START);   // v1.3
  j.raw("}");
}

// "a" message: state of the 6-position accelerometer calibration
static void sendAcal(int num) {
  JBuf j(g_json, sizeof(g_json));
  char miss[48];
  j.raw("{\"t\":\"a\"");
  j.ki("n", g_acal.n);
  j.ki("cov", g_acal.coverage(miss, sizeof(miss)));
  j.ki("cap", g_acal.capturing ? 1 : 0);
  j.ks("msg", g_acal.msg);
  j.key("corr"); j.raw("{");
  j.ki("ok", g_accCorr.valid() ? 1 : 0);
  if (g_accCorr.valid()) {
    j.kv3("off", g_accCorr.offset, 3); j.kv3("gain", g_accCorr.gain, 3);
    j.kf("rms", g_accCorr.rmsMg, 1); j.ki("full", g_accCorr.full);
  }
  j.raw("}}");
  if (num < 0) ws.broadcastTXT(g_json, j.len);
  else ws.sendTXT((uint8_t)num, g_json, j.len);
}

static void sendVerdict() {
  if (!g_verdictOk) return;
  JBuf j(g_json, sizeof(g_json));
  anaWriteJson(j, g_verdict);
  ws.broadcastTXT(g_json, j.len);
}

static void sendMesh() {
  JBuf j(g_json, sizeof(g_json));
  mesh.writeJson(j);
  ws.broadcastTXT(g_json, j.len);
}

static void sendInfo(int num) {
  JBuf j(g_json, sizeof(g_json));
  writeInfo(j);
  if (num < 0) ws.broadcastTXT(g_json, j.len);
  else ws.sendTXT((uint8_t)num, g_json, j.len);
}

// minimal command parser: {"cmd":"age","v":35.5}
static bool jsonGetStr(const char* s, const char* key, char* out, size_t n) {
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(s, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':'); if (!p) return false;
  p = strchr(p, '"'); if (!p) return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
  out[i] = 0;
  return true;
}
static bool jsonGetNum(const char* s, const char* key, float& v) {
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(s, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':'); if (!p) return false;
  v = strtof(p + 1, nullptr);
  return true;
}

static bool jsonGetD(const char* s, const char* key, double& v) {
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(s, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':'); if (!p) return false;
  v = strtod(p + 1, nullptr);
  return true;
}

static void handleCommand(uint8_t num, const char* txt) {
  char cmd[16];
  if (!jsonGetStr(txt, "cmd", cmd, sizeof(cmd))) return;
  float v;
  if (!strcmp(cmd, "age") && jsonGetNum(txt, "v", v)) {
    if (v >= 0.0f && v <= 120.0f) {
      g_ageYears = v;
      engine.setAge(v);
      prefs.putFloat("age", v);
      Serial.printf("[CMD] age = %.2f years (%s)\n", v, ageProfileFor(v).name);
      sendInfo(-1);
    }
  } else if (!strcmp(cmd, "time")) {
    double e;
    if (jsonGetD(txt, "v", e) && e > 1.0e9) lic.offerTime((uint32_t)e, false);
  } else if (!strcmp(cmd, "start")) {             // v1.3: Start measurement (calibrate 8 s, then measure)
    if (g_sensorOk) g_startRequest = true;
  } else if (!strcmp(cmd, "stop")) {              // v1.3: Stop measurement (back to the placement view)
    g_stopRequest = true;
  } else if (!strcmp(cmd, "recal")) {
    if (g_calState == CAL_READY) g_startRequest = true;   // not measuring: recalibrating = starting
    else g_recalRequest = true;
    Serial.println(F("[CMD] recalibration requested"));
  } else if (!strcmp(cmd, "reset")) {
    if (g_calState == CAL_DONE) g_resetRequest = true;          // restart the session with the same calibration
    else if (g_calState == CAL_READY) g_startRequest = true;    // not measuring: a restart is a start
    Serial.println(F("[CMD] session reset"));
  } else if (!strcmp(cmd, "polarity")) {
    engine.flipPolarity();
  } else if (!strcmp(cmd, "info")) {
    sendInfo(num);
    sendAcal(num);
  } else if (!strcmp(cmd, "acal")) {
    char op[12] = "";
    jsonGetStr(txt, "op", op, sizeof(op));
    if (!strcmp(op, "capture")) {
      if (!g_sensorOk || (g_calState != CAL_DONE && g_calState != CAL_READY)) snprintf(g_acal.msg, sizeof(g_acal.msg), "Wait until the sensor is ready.");
      else if (!g_acal.capturing) g_acal.startCapture();
    } else if (!strcmp(op, "clear")) {
      g_acal.clear();
    } else if (!strcmp(op, "save")) {
      AccelCorrection c;
      char msg[160];
      if (g_acal.solve(c, msg, sizeof(msg))) {
        g_accCorrNew = c;
        g_accCorrUpdate = true;
        prefs.putBytes("acorr", &c, sizeof(c));
        g_acal.clear();
        snprintf(g_acal.msg, sizeof(g_acal.msg), "Saved. Attach the sensor to the body and press Recalibrate.");
        Serial.printf("[ACC] %s\n", msg);
      } else {
        strlcpy(g_acal.msg, msg, sizeof(g_acal.msg));
      }
    } else if (!strcmp(op, "remove")) {
      g_accCorrNew = AccelCorrection();
      g_accCorrUpdate = true;
      prefs.remove("acorr");
      snprintf(g_acal.msg, sizeof(g_acal.msg), "Stored accelerometer calibration removed.");
    }
    delay(20);     // let the sensor task pick up a new correction before reporting
    sendAcal(-1);
  }
}

static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      g_lastClientSeenMs = millis();
      sendInfo(num);
      sendAcal(num);
      protoSync(s_cursor, engine, true);   // resend a short history for the new page
      break;
    case WStype_TEXT: {
      char buf[200];
      size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
      memcpy(buf, payload, n); buf[n] = 0;
      handleCommand(num, buf);
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------
//  HTTP: dashboard, JSON APIs, captive-portal probes
// ---------------------------------------------------------------------
struct ChunkWriter {       // buffered chunked-transfer writer for large JSON
  char buf[1460]; size_t len = 0;
  void flush() { if (len) { server.sendContent(buf, len); len = 0; } }   // never sends an empty chunk
  void add(const char* s) {
    size_t n = strlen(s);
    if (n == 0) return;
    if (len + n > sizeof(buf)) flush();
    if (n > sizeof(buf)) { server.sendContent(s, n); return; }
    memcpy(buf + len, s, n); len += n;
  }
  void fmt(const char* f, ...) {
    char t[384];
    va_list ap; va_start(ap, f); vsnprintf(t, sizeof(t), f, ap); va_end(ap);
    add(t);
  }
};

static void handleRoot() {
  // RFC 8908 captive-portal API (DHCP option 114 points here): Android 11+
  // and iOS 14+ ask for JSON and then open the user portal = this dashboard.
  if (server.header("Accept").indexOf("application/captive+json") >= 0) {
    server.sendHeader("Cache-Control", "private");
    server.send(200, "application/captive+json", "{\"captive\":true,\"user-portal-url\":\"http://192.168.4.1/\"}");
    return;
  }
  // v1.3: on the monitor's own network any name (a phone's connectivity check, a typed word) leads to one fixed
  // address, so the sign-in window always opens the same page and the page keeps its settings (theme, alarms)
  if (server.client().localIP() == IPAddress(192, 168, 4, 1)) {
    String h = server.hostHeader();
    h.toLowerCase();
    const int c = h.indexOf(':'); if (c > 0) h = h.substring(0, c);
    const String a = String(net.host()) + ".local", b = String(net.uniqueHost()) + ".local";
    if (h.length() && h != "192.168.4.1" && h != a && h != b) {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.sendHeader("Cache-Control", "no-cache");
      server.send(302, "text/plain", "");
      return;
    }
  }
  // v1.4: the browser keeps the page and only asks "changed?" on reload: 304 in a few ms instead of 116 KB
  //       (the old no-store sent the whole page on every reload, blocking the loop and heating the radio)
  static const String etag = String("\"cr-") + FW_VERSION + "-" + String((unsigned)INDEX_HTML_GZ_LEN) + "\"";
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("ETag", etag);
  if (server.header("If-None-Match") == etag) { server.send(304, "text/plain", ""); return; }
  server.sendHeader("Content-Encoding", "gzip");                 // page is stored compressed: less flash, less radio time
  server.send_P(200, "text/html; charset=utf-8", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

static void handleCaptive() {
  // Any unknown host / path (OS connectivity checks) -> redirect to the
  // dashboard so the phone shows its "sign in to network" page instantly.
  const bool viaAP = server.client().localIP() == IPAddress(192, 168, 4, 1);
  if (!viaAP) {                       // home network: plain "not found" -> dashboard
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-cache");
  server.send(302, "text/plain", "");
}

// ---------------- Wi-Fi setup API ----------------
static void sendJson(int code, const char* body) {
  server.sendHeader("Cache-Control", "no-cache");
  server.send(code, "application/json", body);
}
// (HTTP handlers and WebSocket sends all run in the loop task, so they can
//  share the one g_json buffer)
static void handleWifiStatus() {
  JBuf j(g_json, sizeof(g_json));
  net.writeStatus(j);
  sendJson(200, g_json);
}
static void handleWifiScan() {
  if (server.hasArg("start")) net.startScan();
  JBuf j(g_json, sizeof(g_json));
  net.writeScan(j);
  sendJson(200, g_json);
}
static void handleWifiConnect() {
  NetSettings s;
  strlcpy(s.ssid, server.arg("ssid").c_str(), sizeof(s.ssid));
  strlcpy(s.pass, server.arg("pass").c_str(), sizeof(s.pass));
  String host = server.arg("host");
  host.trim(); host.toLowerCase();
  strlcpy(s.host, host.c_str(), sizeof(s.host));                  // v1.3: empty = automatic simple name (cardio, cardio2 ...)
  s.useStatic = server.arg("static") == "1";
  char err[96] = "";
  if (s.useStatic) {
    if (!net.parseIp(server.arg("ip").c_str(), s.ip) || !net.parseIp(server.arg("gw").c_str(), s.gw) ||
        !net.parseIp(server.arg("mask").c_str(), s.mask)) {
      sendJson(400, "{\"error\":\"Enter valid IP address, gateway and subnet mask\"}");
      return;
    }
    if (server.arg("dns").length()) net.parseIp(server.arg("dns").c_str(), s.dns);
  }
  // keep the saved password when the page leaves it empty for the same network
  if (!s.pass[0] && server.arg("keep") == "1" && !strcmp(s.ssid, net.savedSsid())) {
    Preferences p; p.begin("cardionet", true); p.getString("pass", s.pass, sizeof(s.pass)); p.end();
  }
  if (!net.startTest(s, err, sizeof(err))) {
    snprintf(g_json, sizeof(g_json), "{\"error\":\"%s\"}", err);
    sendJson(400, g_json);
    return;
  }
  sendJson(200, "{\"ok\":1}");
}
static void handleWifiStatic() {
  char msg[96];
  bool ok = net.makeStatic(msg, sizeof(msg));
  snprintf(g_json, sizeof(g_json), ok ? "{\"ok\":1,\"msg\":\"%s\"}" : "{\"error\":\"%s\"}", msg);
  sendJson(ok ? 200 : 400, g_json);
}
// v1.3: other monitors on the home network (mDNS); scan=1 starts a new search (about 3 s, non-blocking)
static void handlePeers() {
  if (server.arg("scan") == "1") net.startPeerScan();
  JBuf j(g_json, sizeof(g_json));
  net.writePeers(j);
  sendJson(200, g_json);
}
// v1.3: keep the monitor's own network (with its captive portal) on while it is on the home network
static void handleApKeep() {
  net.setApKeep(server.arg("on") == "1");
  handleWifiStatus();
}
// v1.3: the bed / room label (mesh label, else the patient's bed) is announced to the other monitors
static void announceLabel() { net.setLabel(mesh.cfg().lbl[0] ? mesh.cfg().lbl : prof.patient.bed); }
static void handleWifiForget() {
  sendJson(200, "{\"ok\":1}");
  delay(50);
  net.forget();
}

static void handleStatusHttp() {
  JBuf j(g_json, sizeof(g_json));
  j.raw("{\"t\":\"s\"");
  j.ki("cal", g_calState); j.ks("calmsg", g_calMsg); j.kf("age", g_ageYears, 2); j.ki("run", g_runFlag ? 1 : 0); j.ki("tcal", g_tcal ? 1 : 0);
  OrientInfo o;
  if (g_calState == CAL_DONE && engine.isRunning()) {
    VitalsResults r; engine.getResults(r);
    if (lic.full()) { protoResults(j, r); fillOrient(o, &r); j.ki("still", o.still); j.ki("osrc", o.src); }
    else { protoResultsLite(j, r); fillOrient(o, &r); protoOrientation(j, o); }
  } else if (g_sensorOk && g_pvValid) { fillOrient(o, nullptr); protoOrientation(j, o); }
  j.ki("full", lic.full() ? 1 : 0);
  j.raw("}");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", g_json);
}

// Full log of the last 10 minutes (trend, beats, breaths, events, breathing
// waveform, 10-s SCG/GCG strip, spectra) streamed as chunked JSON.
static void handleLog() {
  if (!lic.full()) {
    server.send(403, "application/json", "{\"error\":\"licence\"}");
    return;
  }
  if (g_calState != CAL_DONE || !engine.isRunning()) {
    server.send(503, "application/json", "{\"error\":\"measurement not running\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  ChunkWriter w;
  LogMeta m = {g_ssid, FW_VERSION, "chest-worn 6-axis motion sensor", g_ageYears};
  protoLog(w, engine, m, g_json, sizeof(g_json));
  w.flush();
}

// ---------------------------------------------------------------------
//  v1.2 HTTP API: licence, about, profiles, files, recording, mesh, temperature, update
// ---------------------------------------------------------------------
static void sendOk(const char* extra = nullptr) {
  // v1.3 fix: callers often build "extra" inside g_json itself; formatting a buffer into itself is undefined
  // (the reply became invalid JSON, e.g. "update failed" shown after a successful firmware upload)
  static char tmp[320];
  if (extra && extra >= g_json && extra < g_json + sizeof(g_json)) { strlcpy(tmp, extra, sizeof(tmp)); extra = tmp; }
  snprintf(g_json, sizeof(g_json), "{\"ok\":1%s%s}", extra ? "," : "", extra ? extra : "");
  sendJson(200, g_json);
}
static void sendErr(int code, const char* msg) {
  JBuf j(g_json, sizeof(g_json));
  j.raw("{"); j.ks("error", msg); j.raw("}");
  sendJson(code, g_json);
}

static void handleLicenseGet() {
  JBuf j(g_json, sizeof(g_json));
  j.raw("{"); lic.writeJson(j); j.raw("}");
  sendJson(200, g_json);
}
static void handleLicensePost() {
  String tok = server.arg("token");
  tok.trim();
  if (tok.length() < 40 || tok.length() > 420) { sendErr(400, "Paste the complete activation key"); return; }
  char err[96] = "";
  bool ok = lic.activate(tok.c_str(), err, sizeof err);
  JBuf j(g_json, sizeof(g_json));
  j.raw("{"); j.ki("ok", ok ? 1 : 0); j.ks("msg", err[0] ? err : lic.info().msg); lic.writeJson(j); j.raw("}");
  sendJson(ok ? 200 : 400, g_json);
  if (ok) g_infoDirty = true;
}
static void handleLicenseRemove() {
  lic.remove();
  handleLicenseGet();
}
static void handleAbout() {
  JBuf j(g_json, sizeof(g_json));
  j.raw("{");
  j.ks("name", DESIGNER_NAME); j.ks("email", DESIGNER_EMAIL); j.ks("phone", DESIGNER_PHONE); j.ks("wa", DESIGNER_WHATSAPP); j.ks("org", DESIGNER_ORG);
  j.ks("fw", FW_VERSION); j.ks("build", __DATE__ " " __TIME__); j.ks("ml", ML_VERSION); j.ks("dev", g_ssid); j.ks("mac", lic.mac());
  j.ki("flash", (long)ESP.getFlashChipSize()); j.ki("sketch", (long)ESP.getSketchSize()); j.ki("freesketch", (long)ESP.getFreeSketchSpace());
  j.ki("cpu", (long)ESP.getCpuFreqMHz()); j.ks("ota", OTA_PASSWORD[0] ? "password" : "open");
  j.raw("}");
  sendJson(200, g_json);
}
static void handleProfileGet() {
  JBuf j(g_json, sizeof(g_json));
  prof.writeJson(j);
  sendJson(200, g_json);
}
static void handleProfilePost() {
  String kind = server.arg("kind");
  bool ok = prof.save(kind.c_str(), [](const char* k) { return server.arg(k); });
  if (!ok) { sendErr(400, "Unknown profile"); return; }
  announceLabel();
  handleProfileGet();
}
static void handleFiles() {
  JBuf j(g_json, sizeof(g_json));
  rec.listJson(j);
  sendJson(200, g_json);
}
static void handleFileGet() {
  String n = server.arg("n");
  File f;
  if (!rec.openRead(n.c_str(), f)) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + n + "\"");
  server.streamFile(f, "application/octet-stream");
  f.close();
}
static void handleFileDelete() {
  if (server.arg("all") == "1") { int c = rec.removeAll(); snprintf(g_json, sizeof g_json, "\"deleted\":%d", c); sendOk(g_json); return; }
  if (!rec.remove(server.arg("n").c_str())) { sendErr(400, "Cannot delete this file"); return; }
  sendOk();
}
static void handleRec() {
  const String op = server.arg("op");
  if (op == "stop") { rec.stop(true); sendOk(); return; }
  if (op != "start") { sendErr(400, "op must be start or stop"); return; }
  if (g_calState != CAL_DONE || !engine.isRunning()) { sendErr(409, "The measurement is not running yet"); return; }
  int sec = server.arg("sec").toInt();
  if (sec <= 0) sec = REC_DEFAULT_SECONDS;
  RecHeader h; memset(&h, 0, sizeof h);
  h.epoch = lic.now(); h.upMs = millis(); h.age = g_ageYears;
  strlcpy(h.dev, g_devId, sizeof h.dev);
  strlcpy(h.name, prof.patient.name, sizeof h.name); strlcpy(h.pid, prof.patient.id, sizeof h.pid);
  h.full = lic.full() ? 1 : 0;
  char err[96] = "";
  if (!rec.start(engine, sec, h, err, sizeof err)) { sendErr(409, err); return; }
  snprintf(g_json, sizeof g_json, "\"name\":\"%s\",\"sec\":%d", rec.currentName(), rec.total());
  sendOk(g_json);
}
static void handleMeshGet() {
  JBuf j(g_json, sizeof(g_json));
  mesh.writeJson(j);
  sendJson(200, g_json);
}
static void handleMeshPost() {
  const String op = server.arg("op");
  if (op == "on" || op == "off") {
    mesh.setEnabled(op == "on");
    prefs.putBool("mesh", op == "on");
    if (op == "on") { VitalsResults r; bool run = g_calState == CAL_DONE && engine.isRunning(); if (run) engine.getResults(r); meshPublish(run ? &r : nullptr); }
  } else if (op == "refresh" || op == "verify") {
    if (!mesh.enabled()) { sendErr(409, "Mesh is switched off"); return; }
    if (op == "verify") mesh.verifyAll(); else mesh.requestAll();
  } else { sendErr(400, "op must be on, off, refresh or verify"); return; }
  handleMeshGet();
}
static void handleMeshCfgGet() {
  const MeshCfg& c = mesh.cfg();
  JBuf j(g_json, sizeof(g_json));
  j.raw("{"); j.ki("on", mesh.enabled() ? 1 : 0); j.ks("grp", c.grp); j.ki("pinSet", c.pin[0] ? 1 : 0); j.ki("pinLen", (long)strlen(c.pin));
  j.ks("lbl", c.lbl); j.ki("per", c.periodMs); j.ki("ttl", c.ttl); j.ks("id", g_devId); j.ki("ch", (long)WiFi.channel()); j.raw("}");
  sendJson(200, g_json);
}
static void handleMeshCfgPost() {                      // group, pin (empty = keep), pinclear=1, lbl, per (ms), ttl
  MeshCfg c = mesh.cfg();
  if (server.hasArg("grp")) {
    String g = server.arg("grp"); g.trim();
    if (!meshGroupValid(g.c_str())) { sendErr(400, "Group name: 1-15 letters, digits, spaces, - or _"); return; }
    strlcpy(c.grp, g.c_str(), sizeof c.grp);
  }
  if (server.arg("pinclear") == "1") c.pin[0] = 0;
  else if (server.hasArg("pin") && server.arg("pin").length()) {
    const String pin = server.arg("pin");
    if (!meshPinValid(pin.c_str()) || !pin.length()) { sendErr(400, "PIN: 4 to 8 digits"); return; }
    strlcpy(c.pin, pin.c_str(), sizeof c.pin);
  }
  if (server.hasArg("lbl")) {
    String l = server.arg("lbl"); l.trim();
    if (l.length() > 15) { sendErr(400, "Label: at most 15 characters"); return; }
    strlcpy(c.lbl, l.c_str(), sizeof c.lbl);
  }
  if (server.hasArg("per")) { long v = server.arg("per").toInt(); if (v < 1000 || v > 10000) { sendErr(400, "Interval: 1-10 s"); return; } c.periodMs = (uint16_t)v; }
  if (server.hasArg("ttl")) { long v = server.arg("ttl").toInt(); if (v < 0 || v > 3) { sendErr(400, "Relay hops: 0-3"); return; } c.ttl = (uint8_t)v; }
  mesh.configure(c);
  prefs.putString("mgrp", c.grp); prefs.putString("mpin", c.pin); prefs.putString("mlbl", c.lbl);
  prefs.putUShort("mper", c.periodMs); prefs.putUChar("mttl", c.ttl);
  if (server.hasArg("on")) { const bool on = server.arg("on") == "1"; mesh.setEnabled(on); prefs.putBool("mesh", on); }
  { VitalsResults r; bool run = g_calState == CAL_DONE && engine.isRunning(); if (run) engine.getResults(r); meshPublish(run ? &r : nullptr); }
  announceLabel();
  handleMeshCfgGet();
}
static void handlePowerGet() {
  snprintf(g_json, sizeof g_json, "{\"pw\":%d,\"mhz\":%d,\"chip\":%.0f,\"hot\":%d,\"boost\":%d}", (int)g_pwr, (int)ESP.getCpuFreqMHz(), isnan(g_chipTemp) ? 0.0f : g_chipTemp, g_hot ? 1 : 0, g_boostUntil ? 1 : 0);
  sendJson(200, g_json);
}
static void handlePowerPost() {
  int v = server.arg("pw").toInt();
  if (!server.hasArg("pw") || v < 0 || v > 2) { sendErr(400, "pw must be 0, 1 or 2"); return; }
  g_pwr = (uint8_t)v; g_boostUntil = 0;
  prefs.putUChar("pwr", g_pwr);
  applyPower();
  handlePowerGet();
}

static void handleTemp() {
  const String op = server.arg("op");
  if (op == "reset") { g_tempOffsetC = 0; g_tcal = false; }
  else {
    float ref = server.arg("ref").toFloat();
    if (server.arg("unit") == "F") ref = (ref - 32.0f) * 5.0f / 9.0f;
    if (!(ref > 25.0f && ref < 45.0f) || isnan(g_tempC)) { sendErr(400, "Enter the thermometer reading (25-45 degC / 77-113 degF)"); return; }
    g_tempOffsetC = ref - g_tempC;
    g_tcal = true;                         // v1.3: only a calibrated temperature can count as fever
  }
  prefs.putFloat("toff", g_tempOffsetC);
  prefs.putBool("tcal", g_tcal);
  snprintf(g_json, sizeof g_json, "\"offset\":%.2f", g_tempOffsetC);
  sendOk(g_json);
}
static void handleTimePost() {
  double e = server.arg("v").toDouble();
  if (e > 1.0e9) lic.offerTime((uint32_t)e, false);
  sendOk();
}

// ---- v1.3: Start / Stop over HTTP (the page normally uses the WebSocket), restart, factory reset ----
// =====================================================================
//  v1.3: alarm limits (set points) and time windows, changed only by a doctor or nurse (PIN)
// =====================================================================
#ifndef CLINICIAN_PIN_DEFAULT
#define CLINICIAN_PIN_DEFAULT "1234"     // factory PIN for changing the alarm limits - change it on the page
#endif
static char g_pinHash[65] = "";
static bool g_pinIsDefault = true;
static PinGuard g_pinGuard;
struct StaffToken { char t[33]; uint32_t exp; };
static StaffToken g_tok[4];
static const uint32_t STAFF_TOKEN_MS = 30u * 60u * 1000u;      // signed in for 30 minutes

static AlarmLimits alarmAgeDefaults() {
  const AgeProfile& P = ageProfileFor(g_ageYears);
  return alarmDefaults(P.hrLo, P.hrHi, P.rrLo, P.rrHi, P.apneaSec);
}
static float alarmApneaLimit() {                 // used by the status LED's "urgent" flashing
  return g_alm.autoAge ? ageProfileFor(g_ageYears).apneaSec : g_alm.lim.apnea;
}
static void loadAlarms() {
  Preferences p;
  if (!p.begin("cardioalm", true)) { alarmPinHash(g_devId, CLINICIAN_PIN_DEFAULT, g_pinHash); return; }
  AlarmCfg c;
  char err[96];
  if (p.getBytesLength("cfg") == sizeof(AlarmCfg) && p.getBytes("cfg", &c, sizeof c) == sizeof c && c.version == 1 &&
      (c.autoAge || alarmValidate(c.lim, err, sizeof err))) g_alm = c;
  p.getString("pinh", g_pinHash, sizeof g_pinHash);
  p.end();
  g_pinIsDefault = strlen(g_pinHash) != 64;
  if (g_pinIsDefault) alarmPinHash(g_devId, CLINICIAN_PIN_DEFAULT, g_pinHash);
}
static void alarmLog(const char* by, const char* note, uint32_t t) {
  Preferences p;
  if (!p.begin("cardioalm", false)) return;
  AlarmLogEntry L[8];
  memset(L, 0, sizeof L);
  if (p.getBytesLength("log") == sizeof L) p.getBytes("log", L, sizeof L);
  memmove(&L[1], &L[0], sizeof(AlarmLogEntry) * 7);                      // newest first
  L[0].t = t;
  strlcpy(L[0].by, by, sizeof L[0].by);
  strlcpy(L[0].note, note, sizeof L[0].note);
  p.putBytes("log", L, sizeof L);
  p.end();
}
static const char* staffToken() {                // valid token from the request, else nullptr
  const String t = server.arg("token");
  const uint32_t now = millis();
  if (t.length() != 32) return nullptr;
  for (StaffToken& k : g_tok)
    if (k.t[0] && (int32_t)(k.exp - now) > 0 && t == k.t) return k.t;
  return nullptr;
}
static void limitsJson(JBuf& j, const char* key, const AlarmLimits& L) {
  j.key(key); j.raw("{");
  j.kf("hrLoU", L.hrLoU, 0); j.kf("hrLo", L.hrLo, 0); j.kf("hrHi", L.hrHi, 0); j.kf("hrHiU", L.hrHiU, 0);
  j.kf("rrLoU", L.rrLoU, 0); j.kf("rrLo", L.rrLo, 0); j.kf("rrHi", L.rrHi, 0); j.kf("rrHiU", L.rrHiU, 0);
  j.kf("apnea", L.apnea, 0); j.kf("apneaU", L.apneaU, 0);
  j.kf("tLo", L.tLo, 1); j.kf("tHi", L.tHi, 1); j.kf("tHiU", L.tHiU, 1);
  j.kf("hrDly", L.hrDly, 0); j.kf("rrDly", L.rrDly, 0); j.kf("tDly", L.tDly, 0); j.kf("nosigDly", L.nosigDly, 0);
  j.raw("}");
}
static void handleAlarmsGet() {
  JBuf j(g_json, sizeof(g_json));
  j.raw("{");
  j.ki("auto", g_alm.autoAge); j.ki("seq", (long)g_alm.seq); j.ki("at", (long)g_alm.at); j.ks("by", g_alm.by);
  limitsJson(j, "lim", g_alm.autoAge ? alarmAgeDefaults() : g_alm.lim);
  limitsJson(j, "def", alarmAgeDefaults());
  j.ks("group", ageProfileFor(g_ageYears).name); j.kf("age", g_ageYears, 2);
  j.ki("pinDefault", g_pinIsDefault ? 1 : 0); j.ki("locked", (long)g_pinGuard.lockedFor(millis()));
  j.ki("signedIn", staffToken() ? 1 : 0);
  j.key("log"); j.raw("[");
  {
    Preferences p;
    AlarmLogEntry L[8];
    memset(L, 0, sizeof L);
    if (p.begin("cardioalm", true)) { if (p.getBytesLength("log") == sizeof L) p.getBytes("log", L, sizeof L); p.end(); }
    bool first = true;
    for (const AlarmLogEntry& e : L) {
      if (!e.by[0] && !e.note[0]) continue;
      if (!first) j.raw(",");
      first = false;
      j.raw("{"); j.ki("t", (long)e.t); j.ks("by", e.by); j.ks("note", e.note); j.raw("}");
    }
  }
  j.raw("]}");
  sendJson(200, g_json);
}
static void handleAlarmsLogin() {
  const uint32_t now = millis();
  const uint32_t lk = g_pinGuard.lockedFor(now);
  if (lk) { snprintf(g_json, sizeof g_json, "{\"error\":\"Too many wrong PINs - wait %lu s\",\"locked\":%lu}", (unsigned long)lk, (unsigned long)lk); sendJson(429, g_json); return; }
  char h[65];
  alarmPinHash(g_devId, server.arg("pin").c_str(), h);
  if (!alarmHashEq(h, g_pinHash)) { g_pinGuard.fail(now); delay(400); sendErr(403, "Wrong PIN"); return; }
  g_pinGuard.ok();
  StaffToken* slot = &g_tok[0];
  for (StaffToken& k : g_tok) if (!k.t[0] || (int32_t)(k.exp - now) <= 0) { slot = &k; break; }
  if (slot == &g_tok[0] && g_tok[0].t[0] && (int32_t)(g_tok[0].exp - now) > 0)      // all in use: replace the oldest
    for (StaffToken& k : g_tok) if ((int32_t)(k.exp - slot->exp) < 0) slot = &k;
  for (int i = 0; i < 4; i++) { const uint32_t r = esp_random(); snprintf(slot->t + 8 * i, 9, "%08lx", (unsigned long)r); }
  slot->exp = now + STAFF_TOKEN_MS;
  snprintf(g_json, sizeof g_json, "{\"ok\":1,\"token\":\"%s\",\"ttl\":%lu,\"pinDefault\":%d}", slot->t, (unsigned long)(STAFF_TOKEN_MS / 1000), g_pinIsDefault ? 1 : 0);
  sendJson(200, g_json);
}
static void handleAlarmsLogout() {
  const char* t = staffToken();
  if (t) for (StaffToken& k : g_tok) if (k.t == t) { k.t[0] = 0; k.exp = 0; }
  sendOk();
}
static void handleAlarmsPost() {
  if (!staffToken()) { sendErr(401, "Sign in with the doctor / nurse PIN first"); return; }
  String by = server.arg("by"); by.trim();
  if (by.length() < 2) { sendErr(400, "Enter your name (it is kept with the change)"); return; }
  AlarmCfg c = g_alm;
  c.autoAge = server.arg("auto") == "1" ? 1 : 0;
  if (!c.autoAge) {
    AlarmLimits L = g_alm.autoAge ? alarmAgeDefaults() : g_alm.lim;
    struct F { const char* k; float* v; };
    const F f[] = {{"hrLoU", &L.hrLoU}, {"hrLo", &L.hrLo}, {"hrHi", &L.hrHi}, {"hrHiU", &L.hrHiU}, {"rrLoU", &L.rrLoU}, {"rrLo", &L.rrLo},
                   {"rrHi", &L.rrHi}, {"rrHiU", &L.rrHiU}, {"apnea", &L.apnea}, {"apneaU", &L.apneaU}, {"tLo", &L.tLo}, {"tHi", &L.tHi},
                   {"tHiU", &L.tHiU}, {"hrDly", &L.hrDly}, {"rrDly", &L.rrDly}, {"tDly", &L.tDly}, {"nosigDly", &L.nosigDly}};
    for (const F& x : f) if (server.hasArg(x.k)) {
      const String v = server.arg(x.k);
      const char* cs = v.c_str(); char* e = nullptr;
      const float fv = strtof(cs, &e);
      *x.v = (e && e != cs && *e == 0) ? fv : NAN;                     // not a number -> rejected by alarmValidate
    }
    char err[96];
    if (!alarmValidate(L, err, sizeof err)) { sendErr(400, err); return; }
    c.lim = L;
  }
  c.seq = g_alm.seq + 1;
  c.at = (uint32_t)strtoul(server.arg("t").c_str(), nullptr, 10);
  strlcpy(c.by, by.c_str(), sizeof c.by);
  Preferences p;
  if (!p.begin("cardioalm", false) || p.putBytes("cfg", &c, sizeof c) != sizeof c) { p.end(); sendErr(500, "Could not save the alarm limits"); return; }
  p.end();
  g_alm = c;
  String note = server.arg("note"); note.trim();
  alarmLog(c.by, note.length() ? note.c_str() : (c.autoAge ? "age-based defaults" : "limits changed"), c.at);
  Serial.printf("[ALM] alarm limits changed by %s (%s)\n", c.by, c.autoAge ? "age-based defaults" : "custom");
  handleAlarmsGet();
}
static void handleAlarmsPin() {
  if (!staffToken()) { sendErr(401, "Sign in with the doctor / nurse PIN first"); return; }
  const String nw = server.arg("new");
  if (!alarmPinValid(nw.c_str())) { sendErr(400, "The PIN must have 4 to 8 digits"); return; }
  char h[65];
  alarmPinHash(g_devId, nw.c_str(), h);
  Preferences p;
  if (!p.begin("cardioalm", false)) { sendErr(500, "Could not save the PIN"); return; }
  p.putString("pinh", h);
  p.end();
  strlcpy(g_pinHash, h, sizeof g_pinHash);
  g_pinIsDefault = false;
  String by = server.arg("by"); by.trim();
  alarmLog(by.length() ? by.c_str() : "staff", "doctor / nurse PIN changed", (uint32_t)strtoul(server.arg("t").c_str(), nullptr, 10));
  sendOk();
}

static void handleMeasure() {
  const String op = server.arg("op");
  if (op == "start") {
    if (!g_sensorOk) { sendErr(409, "The motion sensor is not connected"); return; }
    g_startRequest = true;
  } else if (op == "stop") g_stopRequest = true;
  else { sendErr(400, "op must be start or stop"); return; }
  snprintf(g_json, sizeof g_json, "\"op\":\"%s\"", op.c_str());
  sendOk(g_json);
}
static void handleReboot() {
  sendOk();
  g_restartAt = millis() + 800;
  Serial.println(F("[SYS] restart requested from the page"));
}
// Factory reset: erases Wi-Fi, age, calibrations, temperature offset, power mode, mesh settings, patient and
// nurse profiles and all recordings. The licence (activation key) is KEPT unless lic=remove is sent.
static void handleFactory() {
  const bool keepLic = server.arg("lic") != "remove";
  sendOk(keepLic ? "\"lic\":\"kept\"" : "\"lic\":\"removed\"");
  Serial.printf("[SYS] factory reset (%s the licence)\n", keepLic ? "keeping" : "removing");
  g_stopRequest = true;
  delay(50);
  if (rec.active()) rec.stop(false);
  rec.removeAll();
  prefs.clear();                                                           // age, calibrations, temperature, power, mesh, run flag
  { Preferences p; if (p.begin("cardionet", false)) { p.clear(); p.end(); } }    // Wi-Fi
  { Preferences p; if (p.begin("cardioprof", false)) { p.clear(); p.end(); } }   // patient / nurse
  { Preferences p; if (p.begin("cardiorec", false)) { p.clear(); p.end(); } }    // recording numbers
  { Preferences p; if (p.begin("cardioalm", false)) { p.clear(); p.end(); } }    // v1.3: alarm limits back to the defaults, factory PIN
  if (!keepLic) lic.remove();
  g_restartAt = millis() + 1500;
}

// ---- firmware update (multipart upload -> OtaUpdate) ----
static void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  if (ota.done()) {
    snprintf(g_json, sizeof g_json, "\"bytes\":%lu,\"kind\":\"%s\"", (unsigned long)ota.bytes(), ota.kind());
    sendOk(g_json);
    g_restartAt = millis() + 1800;
    Serial.printf("[OTA] update written (%lu bytes, %s): restarting\n", (unsigned long)ota.bytes(), ota.kind());
  } else {
    sendErr(400, ota.failed() ? ota.error() : "Update failed");
    Serial.printf("[OTA] failed: %s\n", ota.error());
  }
}
static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    ota.begin();
    if (OTA_PASSWORD[0] && server.arg("key") != OTA_PASSWORD) ota.reject("Wrong update password");
    Serial.printf("[OTA] receiving %s\n", up.filename.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    ota.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    ota.finish();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    ota.abort();
  }
}

// ---- extra Serial commands (called from NetManager::serialPoll) ----
static bool serialHook(const char* line) {
  if (!strncasecmp(line, "lickey ", 7)) {
    char err[64] = "";
    const char* h = line + 7; while (*h == ' ') h++;
    if (lic.setKeyHex(h, err, sizeof err)) { char k[9]; lic.keyCheckValue(k); Serial.printf("[LIC] key stored. Check value %s (must equal the generator's)\n", k); }
    else Serial.printf("[LIC] %s\n", err);
    return true;
  }
  if (!strcasecmp(line, "lic")) {
    const LicenseInfo& i = lic.info();
    Serial.printf("[LIC] MAC %s | key %s | %s | user \"%s\" | expires %lu | now %lu\n", i.mac, i.keySet ? "set" : "NOT set", i.msg, i.user, (unsigned long)i.expiresAt, (unsigned long)lic.now());
    return true;
  }
  if (!strcasecmp(line, "licremove")) { lic.remove(); Serial.println(F("[LIC] activation key removed")); return true; }
  if (!strcasecmp(line, "start")) { if (g_sensorOk) g_startRequest = true; else Serial.println(F("[RUN] no sensor")); return true; }   // v1.3
  if (!strcasecmp(line, "stop")) { g_stopRequest = true; return true; }
  if (!strcasecmp(line, "clockreset")) { lic.resetClockFloor(); Serial.println(F("[LIC] stored time floor cleared")); return true; }
  return false;
}

// ---------------------------------------------------------------------
//  NVS persistence
// ---------------------------------------------------------------------
static void loadSettings() {
  prefs.begin("cardioresp", false);
  g_ageYears = prefs.getFloat("age", DEFAULT_AGE_YEARS);
  g_tempOffsetC = prefs.getFloat("toff", TEMP_OFFSET_C);
  g_tcal = prefs.getBool("tcal", false);                 // v1.3
  g_runFlag = prefs.getBool("run", false);               // v1.3: a measurement was running when the power went off
  g_pwr = prefs.getUChar("pwr", POWER_PROFILE_DEFAULT); if (g_pwr > 2) g_pwr = POWER_PROFILE_DEFAULT;
  if (prefs.getBytesLength("cal") == sizeof(CalibData)) {
    prefs.getBytes("cal", &g_prevCal, sizeof(CalibData));
    g_havePrevCal = true;
  }
  if (prefs.getBytesLength("acorr") == sizeof(AccelCorrection)) {
    prefs.getBytes("acorr", &g_accCorr, sizeof(AccelCorrection));
    if (g_accCorr.valid())
      Serial.printf("[ACC] accelerometer correction loaded: offsets %.3f %.3f %.3f g, gains %.3f %.3f %.3f\n",
                    g_accCorr.offset[0], g_accCorr.offset[1], g_accCorr.offset[2], g_accCorr.gain[0], g_accCorr.gain[1], g_accCorr.gain[2]);
    else g_accCorr = AccelCorrection();
  }
}

// ---------------------------------------------------------------------
//  Setup / loop
// ---------------------------------------------------------------------
// v1.3: look for the sensor on one pair of pins
static bool probeSensorOn(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl, I2C_CLOCK_HZ);
  Wire.setTimeOut(20);
  return mpu.begin(Wire, 0x68) || mpu.begin(Wire, 0x69);
}

void setup() {
  Serial.begin(115200);
#if STATUS_LED_ENABLED
  g_ledPin = PIN_STATUS_LED;
  pinMode(PIN_STATUS_LED, OUTPUT);
  ledWrite(true);
#endif
  delay(600);  // give USB-CDC a moment
  makeCredentials();
  loadAlarms();                                        // v1.3: alarm limits and the doctor / nurse PIN
  loadSettings();
  setCpuFrequencyMhz(PWR[g_pwr].mhz);
  lic.begin();
  prof.begin();
  rec.begin();
  mesh.begin(prefs.getBool("mesh", false));
  {                                                    // v1.2: mesh group, PIN, label, interval and relay hops
    MeshCfg mc;
    if (prefs.isKey("mgrp")) prefs.getString("mgrp", mc.grp, sizeof mc.grp);
    if (!mc.grp[0]) strlcpy(mc.grp, MESH_GROUP_DEFAULT, sizeof mc.grp);
    if (prefs.isKey("mpin")) prefs.getString("mpin", mc.pin, sizeof mc.pin);
    if (prefs.isKey("mlbl")) prefs.getString("mlbl", mc.lbl, sizeof mc.lbl);
    mc.periodMs = prefs.getUShort("mper", MESH_PERIOD_MS); mc.ttl = prefs.getUChar("mttl", MESH_TTL);
    mesh.configure(mc);
  }
  net.setSerialHook(serialHook);
  Serial.printf("[SYS] firmware %s, CPU %d MHz, ML model %s, licence %s\n", FW_VERSION, (int)ESP.getCpuFreqMHz(), ML_VERSION, lic.full() ? "full" : "restricted");
  g_engineMtx = xSemaphoreCreateMutex();
  engine.init();

#if BATTERY_ENABLED
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
#endif

  // ---- sensor (v1.3: first on the configured pins, then on the alternative pins that free the LED and BOOT)
  g_sensorOk = probeSensorOn(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!g_sensorOk && PIN_I2C_SDA_ALT >= 0 && PIN_I2C_SCL_ALT >= 0 && !(PIN_I2C_SDA_ALT == PIN_I2C_SDA && PIN_I2C_SCL_ALT == PIN_I2C_SCL)) {
    if (probeSensorOn(PIN_I2C_SDA_ALT, PIN_I2C_SCL_ALT)) { g_sensorOk = true; g_sdaPin = PIN_I2C_SDA_ALT; g_sclPin = PIN_I2C_SCL_ALT; }
    else probeSensorOn(PIN_I2C_SDA, PIN_I2C_SCL);            // nothing found: keep the main pins for the retries
  }
  {
    const bool ledFree = g_sdaPin != PIN_STATUS_LED && g_sclPin != PIN_STATUS_LED;
    if (g_ledPin < 0 && STATUS_LED_AUTO && g_sensorOk && ledFree) {
      g_ledPin = PIN_STATUS_LED;
      pinMode(g_ledPin, OUTPUT);
      ledWrite(false);
    }
    if (START_BUTTON_AUTO && g_sensorOk && g_sdaPin != PIN_START_BUTTON && g_sclPin != PIN_START_BUTTON && PIN_START_BUTTON != g_ledPin) {
      g_btnPin = PIN_START_BUTTON;
      pinMode(g_btnPin, INPUT_PULLUP);
    }
    Serial.printf("[MPU] sensor pins SDA GPIO%d / SCL GPIO%d | status LED %s | Start button %s\n", g_sdaPin, g_sclPin,
                  g_ledPin >= 0 ? "on" : "off (pin used by the sensor)", g_btnPin >= 0 ? "BOOT" : "off (pin used by the sensor)");
  }
  if (g_sensorOk) {
    // v1.3: wait for Start (placement view), unless a measurement was running when the power went off
    if (g_runFlag || !WAIT_FOR_START) { g_recalRequest = true; g_runFlag = true; }
    else { g_recalRequest = false; g_calState = CAL_READY; snprintf(g_calMsg, sizeof(g_calMsg), "Sensor ready - place it on the chest and press Start"); }
    Serial.printf("[MPU] %s (WHO_AM_I 0x%02X) at 0x%02X, FIFO %d bytes\n", mpu.model(), mpu.whoAmI(), mpu.address(), mpu.fifoSize());
    if (mpu.selfTest(g_selfTest)) {
      Serial.printf("[MPU] self-test %s  accel dev %.1f %.1f %.1f %%  gyro dev %.1f %.1f %.1f %%\n", g_selfTest.passed ? "PASSED" : "OUT OF SPEC",
                    g_selfTest.accDev[0], g_selfTest.accDev[1], g_selfTest.accDev[2], g_selfTest.gyroDev[0], g_selfTest.gyroDev[1], g_selfTest.gyroDev[2]);
      if (!g_selfTest.passed)
        Serial.println(F("[MPU] (common on clone modules, or if the sensor moved during start-up; offsets are corrected automatically -\n"
                         "      the 6-position accelerometer calibration on the page gives the best accuracy)"));
    } else {
      Serial.println(F("[MPU] self-test not available for this chip variant"));
    }
  } else {
    g_calState = CAL_NO_SENSOR;
    g_recalRequest = false;
    snprintf(g_calMsg, sizeof(g_calMsg), "Motion sensor not found - check the sensor cable and connections");
    Serial.printf("[MPU] MPU6050 not found on SDA GPIO%d / SCL GPIO%d nor on GPIO%d / GPIO%d - check wiring\n", PIN_I2C_SDA, PIN_I2C_SCL, PIN_I2C_SDA_ALT, PIN_I2C_SCL_ALT);
  }

  // ---- measurement tasks first: calibration runs while Wi-Fi connects
  xTaskCreate(sensorTask, "sensor", 6144, nullptr, 5, nullptr);
  xTaskCreate(analysisTask, "analysis", 6144, nullptr, 2, nullptr);

  // ---- network: home Wi-Fi if saved and reachable, else own network + captive portal
  net.begin(g_ssid, g_pass, g_host);
  announceLabel();
  applyPower();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/status", HTTP_GET, handleStatusHttp);
  server.on("/api/license", HTTP_GET, handleLicenseGet);
  server.on("/api/license", HTTP_POST, handleLicensePost);
  server.on("/api/license/remove", HTTP_POST, handleLicenseRemove);
  server.on("/api/about", HTTP_GET, handleAbout);
  server.on("/api/profile", HTTP_GET, handleProfileGet);
  server.on("/api/profile", HTTP_POST, handleProfilePost);
  server.on("/api/files", HTTP_GET, handleFiles);
  server.on("/api/file", HTTP_GET, handleFileGet);
  server.on("/api/file/delete", HTTP_POST, handleFileDelete);
  server.on("/api/rec", HTTP_POST, handleRec);
  server.on("/api/mesh", HTTP_GET, handleMeshGet);
  server.on("/api/mesh", HTTP_POST, handleMeshPost);
  server.on("/api/mesh/config", HTTP_GET, handleMeshCfgGet);
  server.on("/api/mesh/config", HTTP_POST, handleMeshCfgPost);
  server.on("/api/temp", HTTP_POST, handleTemp);
  server.on("/api/power", HTTP_GET, handlePowerGet);
  server.on("/api/power", HTTP_POST, handlePowerPost);
  server.on("/api/time", HTTP_POST, handleTimePost);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/api/measure", HTTP_POST, handleMeasure);     // v1.3: op=start | stop
  server.on("/api/reboot", HTTP_POST, handleReboot);       // v1.3
  server.on("/api/factory", HTTP_POST, handleFactory);     // v1.3: lic=remove also removes the activation key
  static const char* const probes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                                       "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink", "/canonical.html",
                                       "/success.txt", "/mobile/status.php", "/check_network_status.txt"};
  for (const char* p : probes) server.on(p, handleCaptive);
  server.on("/api/wifi", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/static", HTTP_POST, handleWifiStatic);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/api/wifi/apkeep", HTTP_POST, handleApKeep);    // v1.3
  server.on("/api/peers", HTTP_GET, handlePeers);            // v1.3
  server.on("/api/alarms", HTTP_GET, handleAlarmsGet);       // v1.3: alarm limits (everybody can read them)
  server.on("/api/alarms", HTTP_POST, handleAlarmsPost);     // v1.3: change them (doctor / nurse token)
  server.on("/api/alarms/login", HTTP_POST, handleAlarmsLogin);
  server.on("/api/alarms/logout", HTTP_POST, handleAlarmsLogout);
  server.on("/api/alarms/pin", HTTP_POST, handleAlarmsPin);
  server.onNotFound(handleCaptive);
  static const char* hdrKeys[] = {"Accept", "If-None-Match"};
  server.collectHeaders(hdrKeys, 2);
  server.begin();

  ws.begin();
  ws.onEvent(onWsEvent);
  ws.enableHeartbeat(5000, 3000, 1);     // v1.4: a reloaded / closed page frees its slot in ~8 s (was ~23 s: reloads found no free slot)

  net.printInfo();
  updateBattery();
}

void loop() {
  net.loop();
  net.serialPoll();
  server.handleClient();
  ws.loop();
  mesh.loop();

  const uint32_t now = millis();
  static uint32_t tWave = 0, tStatus = 0, tSpec = 0, tBatt = 0, tSerial = 0, tCred = 0, tInfo = 0, tSec = 0, tChip = 0, tMesh = 0;
  const bool clients = ws.connectedClients() > 0;
  if (clients) g_lastClientSeenMs = now;

  const bool full = lic.full();
  if (now - tSec >= 1000) { tSec = now; secondTick(now); }
  if (now - tChip >= 5000) { tChip = now; thermalTick(); }
  const PwrCfg& pc = PWR[g_pwr > 2 ? 1 : g_pwr];
  const uint32_t hotX = g_hot ? 2u : 1u;
  if (now - tWave >= hotX * pc.wave) { tWave = now; if (clients && full) sendWave(); }
  // v1.3: while NOT measuring the status (with the placement bubble) is sent twice a second
  const uint32_t statusEvery = g_calState == CAL_DONE ? hotX * pc.status : 500u;
  if (now - tStatus >= statusEvery) { tStatus = now; if (clients) sendStatus(); }
  if (g_runFlagDirty) { g_runFlagDirty = false; prefs.putBool("run", g_runFlag); }   // v1.3: resume after a power cut
  // v1.3: BOOT button (when the sensor is on the alternative pins): a short press starts or stops the measurement
  if (g_btnPin >= 0) {
    static bool btnPrev = false; static uint32_t btnT0 = 0;
    const bool pressed = digitalRead(g_btnPin) == LOW;
    if (pressed && !btnPrev) btnT0 = now;
    if (!pressed && btnPrev) {
      const uint32_t d = now - btnT0;
      if (d >= 40 && d < 3000) {
        if (g_calState == CAL_DONE || g_calState == CAL_RUNNING) g_stopRequest = true;
        else if (g_sensorOk) g_startRequest = true;
        Serial.printf("[BTN] %s\n", g_stopRequest ? "stop" : "start");
      }
    }
    btnPrev = pressed;
  }
  if (now - tSpec >= hotX * pc.spec || g_verdictPush) {
    if (now - tSpec >= hotX * pc.spec) tSpec = now;
    g_verdictPush = false;
    if (clients && full) { sendSpectra(); sendVerdict(); }
  }
  if (now - tMesh >= 2000 && mesh.enabled()) { tMesh = now; if (clients) sendMesh(); }
  if (g_restartAt && (int32_t)(now - g_restartAt) >= 0) { Serial.println(F("[SYS] restarting")); delay(100); ESP.restart(); }
  if (now - tBatt >= 5000) { tBatt = now; updateBattery(); }
  if (g_infoDirty && now - tInfo > 300) { tInfo = now; g_infoDirty = false; if (clients) sendInfo(-1); }
  if (g_calSaveRequest) {
    g_calSaveRequest = false;
    prefs.putBytes("cal", &g_cal, sizeof(CalibData));
  }
  if (!clients && now - tCred >= 30000) { tCred = now; net.printInfo(); }
  if (g_acalDirty) { g_acalDirty = false; if (clients) sendAcal(-1); }

#if SERIAL_PLOTTER
  static uint32_t plotIdx = 0;
  if (g_calState == CAL_DONE) {
    uint32_t h = engine.disp.head;
    if (plotIdx + 50 < h) plotIdx = h - 50;
    for (; plotIdx + 1 < h; plotIdx += 2) {
      const DispSample& d = engine.disp.at(plotIdx);
      Serial.printf("SCG_mg:%.2f,GCG_dps:%.3f,Resp_deg:%.3f\n", d.scg / 100.0f, d.gcg / 1000.0f,
                    engine.resp.head ? engine.resp.at(engine.resp.head - 1) / 1000.0f : 0.0f);
    }
  }
#else
  if (now - tSerial >= 5000) {
    tSerial = now;
    if (g_calState == CAL_DONE) {
      VitalsResults r;
      engine.getResults(r);
      Serial.printf("HR %5.1f bpm (SQI %3u) | RR %5.1f br/min (SQI %3u) | RMSSD %5.1f ms | tilt %4.1f deg | acc offset %.3f g | heap %u | analysis %lu ms | ovf %lu\n",
                    r.hr1m, (unsigned)r.sqiH, r.rr5m, (unsigned)r.sqiR, r.rmssd, r.tiltFromCal,
                    sqrtf(r.accBias[0] * r.accBias[0] + r.accBias[1] * r.accBias[1] + r.accBias[2] * r.accBias[2]),
                    (unsigned)ESP.getFreeHeap(), (unsigned long)(g_analyzeUs / 1000), (unsigned long)g_fifoOverflows);
    } else {
      Serial.printf("[STATE %d] %s (%.0f %%)\n", (int)g_calState, g_calMsg, g_calProgress * 100.0f);
    }
  }
#endif

  // ---- status LED (v1.3): solid = sensor problem, slow blink = ready (place the sensor, press Start),
  //      fast blink = calibrating (keep still), a flash on every heartbeat while measuring,
  //      rapid flashing = urgent alarm (breathing pause beyond the age limit, heart signal lost)
  static uint32_t lastBeatHead = 0, ledOffAt = 0;
  if (g_ledPin >= 0) {
    if (g_calState == CAL_RUNNING) ledWrite((now / 125) & 1);
    else if (g_calState == CAL_NO_SENSOR) ledWrite(true);
    else if (g_calState == CAL_READY || g_calState == CAL_IDLE) ledWrite((now % 1600) < 120);
    else if (g_urgent) ledWrite((now / 70) & 1);
    else {
      if (engine.beats.head != lastBeatHead) {
        lastBeatHead = engine.beats.head;
        VitalsResults r;
        engine.getResults(r);
        if (r.heartPresent && !r.hrArtefact) { ledWrite(true); ledOffAt = now + 60; }
      }
      if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) { ledWrite(false); ledOffAt = 0; }
    }
  }
  // idle polling: slower when nobody is connected (nothing to serve) and in the cooler profiles
  { const bool idle = !clients && !rec.active() && now - g_lastClientSeenMs > 30000UL; delay(idle ? 12 : pc.loopMs); }
}
