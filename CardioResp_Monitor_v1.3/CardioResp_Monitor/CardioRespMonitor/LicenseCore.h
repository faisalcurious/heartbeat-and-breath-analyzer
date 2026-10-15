// LicenseCore.h - portable helpers of the licence check (base64url, flat JSON lookup).
// No Arduino dependencies, so host_test/license_test.cpp can exercise them on a PC.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static inline int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}
// base64 / base64url decode; ignores whitespace and '=' padding. Returns length or -1.
static inline int b64decode(const char* s, uint8_t* out, size_t cap) {
  uint32_t acc = 0; int bits = 0; size_t n = 0;
  for (; *s; s++) {
    char c = *s;
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '=') continue;
    int v = b64val(c);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return -1;
      out[n++] = (uint8_t)(acc >> bits);
      acc &= (1u << bits) - 1u;
    }
  }
  return (int)n;
}
static inline int hexv(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }

// minimal JSON field lookup (the plaintext is flat and compact); order independent
static inline const char* jfind(const char* js, const char* key) {
  char pat[32]; snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(js, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ') p++;
  if (*p != ':') return nullptr;
  p++;
  while (*p == ' ') p++;
  return p;
}
static inline bool jstr(const char* js, const char* key, char* out, size_t cap) {
  const char* p = jfind(js, key);
  if (!p || *p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      char e = *p++;
      if (!e) break;
      if (e == 'u') {
        unsigned cp = 0; for (int i = 0; i < 4 && *p; i++, p++) { int h = hexv(*p); if (h < 0) break; cp = cp * 16 + h; }
        uint8_t u[3]; int k = 0;
        if (cp < 0x80) u[k++] = (uint8_t)cp; else if (cp < 0x800) { u[k++] = 0xC0 | (cp >> 6); u[k++] = 0x80 | (cp & 63); }
        else { u[k++] = 0xE0 | (cp >> 12); u[k++] = 0x80 | ((cp >> 6) & 63); u[k++] = 0x80 | (cp & 63); }
        for (int i = 0; i < k && n + 1 < cap; i++) out[n++] = (char)u[i];
        continue;
      }
      c = e == 'n' ? ' ' : e == 't' ? ' ' : e;
    }
    if (n + 1 < cap) out[n++] = c;
  }
  out[n] = 0;
  return true;
}
static inline bool jnum(const char* js, const char* key, double& v) {
  const char* p = jfind(js, key);
  if (!p || (!isdigit((unsigned char)*p) && *p != '-')) return false;
  v = strtod(p, nullptr);
  return true;
}


// ---- the token check itself, with the AES-GCM primitive injected (mbedTLS on the device, OpenSSL in the PC test)
struct LicFields { char user[48]; char uid[24]; char mac[24]; uint32_t iat, exp; };
typedef bool (*LicGcmFn)(const uint8_t key[16], const uint8_t iv[12], const uint8_t* ct, size_t n, const uint8_t tag[16], uint8_t* out);
// returns true when the token decrypts, authenticates and parses; the caller then compares f.mac and the clock
static inline bool licenseDecode(const uint8_t key[16], const char* token, LicGcmFn gcm, LicFields& f, char* err, size_t errLen) {
  static uint8_t buf[300], plain[260];
  int n = b64decode(token, buf, sizeof buf);
  if (n < 0) { snprintf(err, errLen, "The key contains invalid characters"); return false; }
  if (n < 12 + 16 + 2) { snprintf(err, errLen, "The key is too short"); return false; }
  const size_t ctLen = (size_t)n - 12 - 16;
  if (ctLen > 256) { snprintf(err, errLen, "The key is too long"); return false; }
  if (!gcm(key, buf, buf + 12, ctLen, buf + n - 16, plain)) { snprintf(err, errLen, "Key rejected (wrong secret or altered key)"); return false; }
  plain[ctLen] = 0;
  const char* js = (const char*)plain;
  double ver = 0, iat = 0, exp = 0;
  if (!jnum(js, "version", ver) || (int)ver != 1) { snprintf(err, errLen, "Unsupported key version"); return false; }
  f.user[0] = f.uid[0] = f.mac[0] = 0;
  if (!jstr(js, "mac", f.mac, sizeof f.mac) || !jnum(js, "issuedAt", iat) || !jnum(js, "expiresAt", exp)) { snprintf(err, errLen, "The key is incomplete"); return false; }
  jstr(js, "userName", f.user, sizeof f.user);
  jstr(js, "userId", f.uid, sizeof f.uid);
  f.iat = (uint32_t)iat; f.exp = (uint32_t)exp;
  return true;
}
