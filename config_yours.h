// =====================================================================
//  config.h  -  user settings for the CardioResp monitor
// =====================================================================
#pragma once

#define FW_VERSION            "1.4.0"

// ---------------- Wiring (ESP32-C3 Super Mini) -----------------------
// MPU6050  VCC -> 3V3, GND -> GND, SDA -> GPIO8, SCL -> GPIO9, AD0 -> GND
// GPIO8/9 are the ESP32-C3 default I2C pins. They are also strapping pins
// (and GPIO8 drives the on-board blue LED, GPIO9 the BOOT button); the
// MPU6050 board's pull-up resistors keep both high at power-up, so the
// board boots normally. Do not press BOOT while measuring (it pulls SCL low).
#define PIN_I2C_SDA           8
#define PIN_I2C_SCL           9
#define I2C_CLOCK_HZ          400000      // fast mode; FIFO bursts need it

// The on-board LED shares GPIO8 with SDA, so the status LED must stay
// disabled with this wiring (the LED will simply flicker with I2C traffic).
// v1.3: wire the sensor to GPIO6 / GPIO7 instead and the LED is used automatically
// (see "Second sensor wiring" at the end of this file).
#define STATUS_LED_ENABLED    0
#define PIN_STATUS_LED        8           // on-board blue LED
#define STATUS_LED_ACTIVE_LOW 1
#if STATUS_LED_ENABLED && (PIN_STATUS_LED == PIN_I2C_SDA || PIN_STATUS_LED == PIN_I2C_SCL)
#error "PIN_STATUS_LED is also an I2C pin - set STATUS_LED_ENABLED to 0"
#endif

// ---------------- Battery monitor (optional) -------------------------
// Li-ion cell (+) -> 100k -> GPIO3 -> 100k -> GND   (2:1 divider)
// Leave BATTERY_ENABLED at 1 even without the divider: the page then
// shows "USB power" when no cell voltage is sensed.
#define BATTERY_ENABLED       1
#define PIN_BATTERY_ADC       3           // ADC1 channel 3
#define BATTERY_DIVIDER_RATIO 2.0f        // (R1 + R2) / R2
#define BATTERY_CAL_FACTOR    1.00f       // trim after comparing with a multimeter

// ---------------- Direct network (own access point) -------------------
// Every board gets its own network:  SSID = "CardioResp-XXXXXX" (last 3
// bytes of the chip MAC) and a 10-character password derived from the MAC
// and the secret salt below. Both are printed on the Serial Monitor at
// boot and every 30 s until a phone connects - write them on the device.
#define AP_SSID_PREFIX        "CardioResp-"
#define AP_PASSWORD_SALT      "12345678"   // make it unique for your devices
#define AP_FIXED_PASSWORD     "12345678"          // non-empty (>= 8 chars) = same password on all boards
#define AP_CHANNEL            6
#define AP_MAX_CLIENTS        4
// Many ESP32-C3 Super Mini boards have a poorly matched antenna and fail
// to hold a connection at full power. 8.5 dBm is plenty for a phone next
// to the wearer and also saves battery.
#define WIFI_LOW_TX_POWER     1

// ---------------- Home / clinic Wi-Fi (station mode) ------------------
// Normally entered on the web page (or type "setup" in the Serial Monitor)
// and stored in flash. You may also fill it in here; the page setting wins.
#define HOME_WIFI_SSID        ""
#define HOME_WIFI_PASSWORD    ""
#define DEVICE_HOSTNAME       ""          // "" = automatic simple name: cardio.local, cardio2.local ... (v1.3); a fixed name otherwise
#define STA_CONNECT_TIMEOUT_S 15          // try the home network this long at start-up
#define STA_FALLBACK_S        45          // home network gone this long -> direct network on
#define STA_RETRY_S           120         // retry the home network while the direct one is unused
#define AP_LINGER_S           120         // keep the direct network after setup (to read the address)
// v1.3 network options
#define NAME_BASE             "cardio"    // simple local names: cardio, cardio2, cardio3 ... (the first free one on the network is taken)
#define AP_ALWAYS_ON          1           // 1 = the monitor's own Wi-Fi (with the self-opening page) stays on next to the home Wi-Fi
                                          //     (can be switched on the page: Settings -> Network); 0 = only when the home Wi-Fi is missing
#define STA_DOWN_AP_S         8           // home Wi-Fi lost while the own network is on: stop the reconnect scanning after this many s
// v1.3 alarm limits: everybody sees them on Settings -> Alarm limits; changing them needs this doctor / nurse PIN.
// The page asks to change it on first use (it is then stored, hashed, on the monitor; a factory reset brings this one back).
#define CLINICIAN_PIN_DEFAULT "1234"

#define WEBSOCKET_PORT        81

// ---------------- Measurement ----------------------------------------
#define DEFAULT_AGE_YEARS     30.0f       // used until the page sends an age
#define CALIBRATION_SECONDS   8           // stillness period at every start-up
#define CALIBRATION_ATTEMPTS  3

// ---------------- Debug output ---------------------------------------
// 0 = status summary every 5 s, 1 = Arduino Serial Plotter stream
// (SCG, GCG, breathing at 50 Hz)
#define SERIAL_PLOTTER        0

// =====================================================================
//  v1.2 options
// =====================================================================

// ---------------- Designer / support details (shown on the About page) ----------
// Fill these in before you build. WhatsApp: international format, digits only (no + or spaces).
#define DESIGNER_NAME         "Faisal Ali"
#define DESIGNER_EMAIL        "faisalcurious@egmail.com"
#define DESIGNER_PHONE        "+92 301 7155946"
#define DESIGNER_WHATSAPP     "923017155946"
#define DESIGNER_ORG          "CardioResp Monitor"

// ---------------- Licence (activation key from esp32-activation-keygen.html) ----------
// 1 = full analysis (spectra, rule-based verdict, ML) needs a valid, unexpired key bound to this board.
//     Without it, or after expiry, only heart rate and breathing rate are shown.
// 0 = licence check off (development): everything is always available.
#define LICENSE_ENABLED       1
// 16-byte AES key = PBKDF2-HMAC-SHA256(master secret, "esp32-vibmeter/license/v1", 100000 iterations)
// as 32 hex digits - compute it once on a PC with the one-liner in the generator page header.
// You can also store it at run time from the Serial Monitor:  lickey <32 hex digits>   (kept in flash).
// Leave "" here to rely on the stored key. The key is never sent to the browser.
#define LICENSE_AES_KEY_HEX   "7ad3d58238df7f32b82103a413ab3fb6"

// ---------------- Firmware update from the Settings page ------------------------
// Accepts the sketch .bin and the "merged" .bin. Empty = no password.
#define OTA_PASSWORD          ""

// ---------------- Mesh (ESP-NOW, no router needed) -------------------------------
#define MESH_AVAILABLE        1           // compile the mesh code
#define MESH_PERIOD_MS        2000        // status broadcast interval of every node
#define MESH_TTL              2           // hops a packet may be relayed (0 = no relay)
#define MESH_MAX_NODES        16
// Group and PIN at first start (both can be changed on Settings -> Mesh network). Monitors only accept each
// other's data when the group name AND the PIN are the same; packets are signed with a key made from both.
// An empty PIN means an open mesh (any monitor with the same group name is accepted).
#define MESH_GROUP_DEFAULT    "ward"
#define MESH_PIN_DEFAULT      ""

// ---------------- Recording to flash ---------------------------------------------
#define REC_DEFAULT_SECONDS   60
#define REC_MAX_SECONDS       120
#define REC_KEEP_FREE_BYTES   40000       // refuse to start when less flash is free

// ---------------- Power / heat ---------------------------------------------------
// The ESP32-C3 Super Mini heats up with Wi-Fi + CPU load. These keep it cooler:
// Power profile at first start (the user can change it later on Settings -> Power and heat mode; the choice is kept in flash):
//   0 Performance : 160 MHz, 10 Hz traces, radio 8.5 dBm   (smoothest, warmest)
//   1 Cool        : 80 MHz,  5 Hz traces,  radio 5 dBm     (default; recommended for the Super Mini)
//   2 Eco         : 80 MHz,  2.5 Hz traces, radio 2 dBm    (coolest; keep the phone within ~3 m)
// In Cool/Eco the clock jumps to 160 MHz for 5 minutes by itself if the analysis gets slow or FIFO samples are lost.
#define POWER_PROFILE_DEFAULT 1
// Clock used by the Performance profile (the boost uses 160 MHz as well):
#define CPU_MHZ               160         // 80 / 120 / 160 - check "analysis time" on the System card stays < 400 ms
#define WAVE_INTERVAL_MS      100         // live-trace push interval (50 = smoother but ~2x radio traffic)
#define THERMAL_LIMIT_C       70.0f       // chip temperature: above this the device lowers radio/stream load (v1.4: 80 -> 70, runs cooler)
#define STA_MODEM_SLEEP       1           // v1.4: on (cooler). 1 = Wi-Fi modem sleep in home-Wi-Fi mode (cooler, ~100 ms more latency)

// ---------------- Body temperature (MPU6050 die temperature) -----------------------
// The page shows the sensor temperature in degC and degF. After pressing the sensor to the skin for a few
// minutes, enter a thermometer reading on the Settings page to store an offset (default 0).
#define TEMP_OFFSET_C         0.0f

// =====================================================================
//  v1.3 options
// =====================================================================

// ---------------- Start button and power cuts ---------------------------------------
// 1 = after switching on, the monitor shows the sensor placement and gravity and waits for
//     "Start measurement" on the Live page (or a short press of the BOOT button, see below).
//     If the power goes off DURING a measurement, the measurement starts again by itself.
// 0 = measure straight after every start-up (as in v1.2).
#define WAIT_FOR_START        1

// ---------------- Second sensor wiring (frees the blue LED and the BOOT button) ------
// The monitor looks for the sensor on PIN_I2C_SDA / PIN_I2C_SCL first and then on these pins.
// Move the sensor wires to SDA -> GPIO6 and SCL -> GPIO7 and:
//   * the on-board blue LED (GPIO8) shows the state: slow blink = ready (place the sensor and press
//     Start), fast blink = calibrating (keep still), a flash on every heartbeat while measuring,
//     rapid flashing = urgent alarm (breathing pause or heart signal lost) - visible even without a phone;
//   * a short press on the BOOT button (GPIO9) starts or stops the measurement.
// With the original wiring (GPIO8/9) everything works as before and the LED stays off.
// Set both to -1 to switch the second search off.
#define PIN_I2C_SDA_ALT       6
#define PIN_I2C_SCL_ALT       7
#define STATUS_LED_AUTO       1           // use the on-board LED when GPIO8 is free
#define START_BUTTON_AUTO     1           // use BOOT (GPIO9) as Start / Stop when it is free
