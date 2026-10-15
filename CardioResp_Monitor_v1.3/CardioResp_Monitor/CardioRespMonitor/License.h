// =====================================================================
//  License.h  -  activation key check (token format v1 of the generator)
//
//  token = base64url( IV[12] || AES-128-GCM ciphertext || tag[16] )
//  plaintext = {"userName","userId","mac","issuedAt","expiresAt","version":1}
//  Checks in the order the generator page prescribes: decode -> length ->
//  authenticated decrypt -> JSON -> version -> MAC binding -> expiry.
//
//  Clock: the device has no RTC. Time comes from the phone (the page sends
//  it on every connect) or SNTP. A stored "latest time ever seen" prevents
//  setting the phone clock back to extend a key; the expiry check waits
//  until a trustworthy time exists (the key counts as valid until then).
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "Protocol.h"

struct LicenseInfo {
  bool hasToken = false;       // a token is stored
  bool authentic = false;      // decrypts and authenticates with our key
  bool macOk = false;          // bound to this board
  bool expired = false;        // clock says expiry reached
  bool clockKnown = false;     // trustworthy time available
  bool keySet = false;         // an AES key is configured
  char user[48] = "";
  char userId[24] = "";
  char mac[18] = "";           // this board's MAC as the generator wants it
  uint32_t issuedAt = 0, expiresAt = 0;
  char msg[96] = "";           // human readable state
};

class License {
 public:
  void begin();
  void tick();                                     // call about once per second
  bool full() const { return !m_enabled || (m_i.authentic && m_i.macOk && !m_i.expired); }
  bool enabled() const { return m_enabled; }
  const LicenseInfo& info() const { return m_i; }
  bool activate(const char* token, char* err, size_t errLen);
  void remove();
  void offerTime(uint32_t epoch, bool trusted);    // phone / SNTP time
  uint32_t now() const;                            // 0 = unknown
  bool setKeyHex(const char* hex, char* err, size_t errLen);   // Serial only
  void keyCheckValue(char out[9]) const;           // first 4 bytes of SHA-256("KCV"||key)
  void writeJson(JBuf& j);                         // for /api/license and the status message
  const char* mac() const { return m_i.mac; }
  void resetClockFloor() { m_floor = 0; m_lastPersisted = 0; m_baseEpoch = 0; m_p.remove("seen"); evaluate(); }

 private:
  bool verify(const char* token, LicenseInfo& out, char* err, size_t errLen);
  void evaluate();
  void persistSeen(bool force);
  bool loadKey();
  Preferences m_p;
  bool m_enabled = true;
  uint8_t m_key[16]; bool m_haveKey = false;
  LicenseInfo m_i;
  uint32_t m_floor = 0;                            // latest time ever seen (persisted)
  uint32_t m_baseEpoch = 0, m_baseMs = 0;          // current software clock
  bool m_trusted = false;
  uint32_t m_lastPersistMs = 0, m_lastPersisted = 0;
};
