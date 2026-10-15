#include "License.h"
#include "LicenseCore.h"
#include "config.h"
#ifndef LICENSE_MASTER_SECRET
#define LICENSE_MASTER_SECRET ""      // older config.h files do not define it
#endif
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "esp_mac.h"
#include <time.h>

// ---- License ---------------------------------------------------------------------------------
void License::begin() {
  m_enabled = LICENSE_ENABLED != 0;
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(m_i.mac, sizeof m_i.mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  m_p.begin("cardiolic", false);
  m_floor = m_p.getUInt("seen", 0);
  m_lastPersisted = m_floor;
  m_haveKey = loadKey();
  m_i.keySet = m_haveKey;
  if (m_haveKey) {
    char kcv[9]; keyCheckValue(kcv);
    Serial.printf("[LIC] key check value %s (compare with the generator page)\n", kcv);
  } else if (m_enabled) {
    Serial.println(F("[LIC] no AES key configured: set LICENSE_MASTER_SECRET (or LICENSE_AES_KEY_HEX) in config.h, or type  lickey <32 hex>  here."));
  }
  String tok = m_p.getString("tok", "");
  if (tok.length()) {
    m_i.hasToken = true;
    char err[96] = "";
    LicenseInfo t; strlcpy(t.mac, m_i.mac, sizeof t.mac);
    if (verify(tok.c_str(), t, err, sizeof err)) { t.hasToken = true; t.keySet = m_haveKey; m_i = t; }
    else { strlcpy(m_i.msg, err, sizeof m_i.msg); }
  }
  evaluate();
  Serial.printf("[LIC] MAC %s  %s\n", m_i.mac, m_i.msg);
}

// PBKDF2-HMAC-SHA256, one output block (dkLen <= 32), same as the generator page / Python hashlib.pbkdf2_hmac
static bool pbkdf2Sha256(const char* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, uint32_t iter, uint8_t* out, size_t outLen) {
  if (outLen > 32) return false;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t c; mbedtls_md_init(&c);
  bool ok = mbedtls_md_setup(&c, info, 1) == 0 && mbedtls_md_hmac_starts(&c, (const uint8_t*)pw, pwLen) == 0;
  uint8_t u[32], t[32], blk[64];
  if (ok) {
    memcpy(blk, salt, saltLen); blk[saltLen] = 0; blk[saltLen + 1] = 0; blk[saltLen + 2] = 0; blk[saltLen + 3] = 1;
    ok = mbedtls_md_hmac_update(&c, blk, saltLen + 4) == 0 && mbedtls_md_hmac_finish(&c, u) == 0;
    memcpy(t, u, 32);
    for (uint32_t i = 1; ok && i < iter; i++) {
      ok = mbedtls_md_hmac_reset(&c) == 0 && mbedtls_md_hmac_update(&c, u, 32) == 0 && mbedtls_md_hmac_finish(&c, u) == 0;
      for (int k = 0; k < 32; k++) t[k] ^= u[k];
    }
  }
  mbedtls_md_free(&c);
  if (ok) memcpy(out, t, outLen);
  return ok;
}

bool License::loadKey() {
  const char* h = LICENSE_AES_KEY_HEX;
  if (strlen(h) == 32) {
    bool ok = true;
    for (int i = 0; i < 16; i++) { int a = hexv(h[2 * i]), b = hexv(h[2 * i + 1]); if (a < 0 || b < 0) { ok = false; break; } m_key[i] = (uint8_t)(a * 16 + b); }
    if (ok) return true;
  }
  // v1.2: master secret in config.h. The 16-byte key is derived once (about 1-3 s) and cached in flash;
  // it is derived again only if the secret in config.h changes.
  const char* sec = LICENSE_MASTER_SECRET;
  if (sec[0]) {
    uint8_t fp[32], in[3 + 96]; size_t sl = strlen(sec); if (sl > 96) sl = 96;
    memcpy(in, "SRC", 3); memcpy(in + 3, sec, sl); mbedtls_sha256(in, 3 + sl, fp, 0);
    uint8_t oldFp[8];
    if (m_p.getBytesLength("key") == 16 && m_p.getBytesLength("ksrc") == 8) {
      m_p.getBytes("ksrc", oldFp, 8);
      if (!memcmp(oldFp, fp, 8)) { m_p.getBytes("key", m_key, 16); return true; }
    }
    Serial.println(F("[LIC] deriving the licence key from LICENSE_MASTER_SECRET (once, a few seconds)..."));
    static const uint8_t salt[] = "esp32-vibmeter/license/v1";
    if (pbkdf2Sha256(sec, strlen(sec), salt, sizeof(salt) - 1, 100000, m_key, 16)) {
      m_p.putBytes("key", m_key, 16); m_p.putBytes("ksrc", fp, 8);
      return true;
    }
    Serial.println(F("[LIC] key derivation failed"));
  }
  if (m_p.getBytesLength("key") == 16) { m_p.getBytes("key", m_key, 16); return true; }
  return false;
}

bool License::setKeyHex(const char* hex, char* err, size_t n) {
  if (strlen(hex) != 32) { snprintf(err, n, "Need exactly 32 hex digits"); return false; }
  uint8_t k[16];
  for (int i = 0; i < 16; i++) { int a = hexv(hex[2 * i]), b = hexv(hex[2 * i + 1]); if (a < 0 || b < 0) { snprintf(err, n, "Not hexadecimal"); return false; } k[i] = (uint8_t)(a * 16 + b); }
  m_p.putBytes("key", k, 16);
  memcpy(m_key, k, 16); m_haveKey = true; m_i.keySet = true;
  String tok = m_p.getString("tok", "");
  if (tok.length()) { LicenseInfo t; strlcpy(t.mac, m_i.mac, sizeof t.mac); char e[96] = ""; if (verify(tok.c_str(), t, e, sizeof e)) { t.hasToken = true; t.keySet = true; m_i = t; } }
  evaluate();
  return true;
}

void License::keyCheckValue(char out[9]) const {
  uint8_t in[3 + 16] = {'K', 'C', 'V'}, h[32];
  memcpy(in + 3, m_key, 16);
  mbedtls_sha256(in, sizeof in, h, 0);
  snprintf(out, 9, "%02X%02X%02X%02X", h[0], h[1], h[2], h[3]);
}

static bool gcmMbed(const uint8_t key[16], const uint8_t iv[12], const uint8_t* ct, size_t n, const uint8_t tag[16], uint8_t* out) {
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&gcm, n, iv, 12, nullptr, 0, tag, 16, ct, out);
  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

bool License::verify(const char* token, LicenseInfo& out, char* err, size_t errLen) {
  if (!m_haveKey) { snprintf(err, errLen, "No licence key is configured on this device"); return false; }
  LicFields f;
  if (!licenseDecode(m_key, token, gcmMbed, f, err, errLen)) return false;
  out.authentic = true;
  strlcpy(out.user, f.user, sizeof out.user);
  strlcpy(out.userId, f.uid, sizeof out.userId);
  out.issuedAt = f.iat; out.expiresAt = f.exp;
  out.macOk = (strcmp(f.mac, out.mac) == 0);
  if (!out.macOk) { snprintf(err, errLen, "This key was made for another board (%s)", f.mac); return true; }   // authentic, but not for us
  err[0] = 0;
  return true;
}

void License::evaluate() {
  const uint32_t t = now();
  m_i.clockKnown = t != 0 && t >= 1735689600u;                     // not before 2025-01-01
  m_i.expired = false;
  if (!m_enabled) { strlcpy(m_i.msg, "Licence check is disabled in config.h (development build).", sizeof m_i.msg); return; }
  if (!m_haveKey) { strlcpy(m_i.msg, "No licence key is configured in the firmware.", sizeof m_i.msg); return; }
  if (!m_i.hasToken) { strlcpy(m_i.msg, "Not activated. Only heart rate and breathing rate are shown.", sizeof m_i.msg); return; }
  if (!m_i.authentic) { if (!m_i.msg[0]) strlcpy(m_i.msg, "The stored key is not valid.", sizeof m_i.msg); return; }
  if (!m_i.macOk) { strlcpy(m_i.msg, "The stored key belongs to another board.", sizeof m_i.msg); return; }
  if (m_i.clockKnown && t >= m_i.expiresAt) { m_i.expired = true; strlcpy(m_i.msg, "Licence expired. Only heart rate and breathing rate are shown.", sizeof m_i.msg); return; }
  if (m_i.clockKnown && t < m_i.issuedAt) { strlcpy(m_i.msg, "Licensed (waiting for a valid clock).", sizeof m_i.msg); return; }
  strlcpy(m_i.msg, m_i.clockKnown ? "Licensed." : "Licensed (expiry is checked once the phone sends the time).", sizeof m_i.msg);
}

uint32_t License::now() const {
  uint32_t t = 0;
  if (m_baseEpoch) t = m_baseEpoch + (millis() - m_baseMs) / 1000u;
  if (t < m_floor) t = m_floor + (m_baseEpoch ? 0 : millis() / 1000u);   // never go behind the latest time seen
  return t;
}

void License::offerTime(uint32_t e, bool trusted) {
  if (e < 1735689600u) return;                                         // before 2025: not a real clock
  if (!trusted) {
    if (m_floor && e + 120u < m_floor) return;                         // phone clock set back: ignore
    if (m_floor && e > m_floor + 400u * 86400u) return;                // absurd jump forward: ignore
  }
  m_baseEpoch = e; m_baseMs = millis(); m_trusted = trusted;
  if (e > m_floor) { m_floor = e; persistSeen(false); }
  evaluate();
}

void License::persistSeen(bool force) {
  const uint32_t t = millis();
  if (!force && t - m_lastPersistMs < 600000u && m_lastPersisted) return;   // flash wear: at most every 10 min
  m_lastPersistMs = t;
  uint32_t cur = now();
  if (cur > m_lastPersisted) { m_p.putUInt("seen", cur); m_lastPersisted = cur; }
}

void License::tick() {
  const uint32_t t = now();
  if (t > m_floor && m_baseEpoch) m_floor = t;
  if (millis() - m_lastPersistMs > 600000u && m_baseEpoch) persistSeen(true);
  const bool wasExpired = m_i.expired;
  evaluate();
  if (m_i.expired && !wasExpired) { persistSeen(true); Serial.println(F("[LIC] licence expired")); }
}

bool License::activate(const char* token, char* err, size_t errLen) {
  LicenseInfo t; strlcpy(t.mac, m_i.mac, sizeof t.mac);
  t.keySet = m_haveKey;
  if (!verify(token, t, err, errLen)) return false;
  if (!t.macOk) return false;                                        // err already explains
  m_p.putString("tok", token);
  t.hasToken = true;
  m_i = t;
  evaluate();
  if (m_i.expired) { snprintf(err, errLen, "This key is valid but already expired"); return true; }
  err[0] = 0;
  Serial.printf("[LIC] activated for %s until %lu\n", m_i.user, (unsigned long)m_i.expiresAt);
  return true;
}

void License::remove() {
  m_p.remove("tok");
  LicenseInfo t; strlcpy(t.mac, m_i.mac, sizeof t.mac); t.keySet = m_haveKey; m_i = t;
  evaluate();
}

void License::writeJson(JBuf& j) {
  j.ki("en", m_enabled ? 1 : 0); j.ki("full", full() ? 1 : 0);
  j.ki("tok", m_i.hasToken ? 1 : 0); j.ki("auth", m_i.authentic ? 1 : 0); j.ki("macok", m_i.macOk ? 1 : 0);
  j.ki("exp", m_i.expired ? 1 : 0); j.ki("clk", m_i.clockKnown ? 1 : 0); j.ki("key", m_haveKey ? 1 : 0);
  j.ks("mac", m_i.mac); j.ks("user", m_i.user); j.ks("uid", m_i.userId);
  j.ki("iat", (long)m_i.issuedAt); j.ki("eat", (long)m_i.expiresAt); j.ki("now", (long)now());
  j.ks("msg", m_i.msg);
}
