// =====================================================================
//  MeshAuth.h - group code and packet signature for the monitor mesh
//
//  group code : FNV-1a 32-bit hash of the group name (lower case). Not secret;
//               it only tells monitors of different groups apart.
//  key        : SHA-256("CardioResp-mesh/v2|" + group + "|" + PIN)
//  tag        : first 8 bytes of HMAC-SHA256(key, packet) computed with the
//               hop fields (ttl, hops) and the tag itself set to zero, so a
//               relayed packet keeps its original signature.
//  A monitor accepts data only from packets whose tag matches its own group
//  and PIN. An empty PIN is allowed ("open" mesh) but is shown as such.
// =====================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#ifdef MESH_AUTH_OPENSSL                        // PC test build
#include <openssl/hmac.h>
#include <openssl/sha.h>
static inline void meshSha256(const uint8_t* d, size_t n, uint8_t out[32]) { SHA256(d, n, out); }
static inline void meshHmac(const uint8_t key[32], const uint8_t* d, size_t n, uint8_t out[32]) {
  unsigned int l = 32; HMAC(EVP_sha256(), key, 32, d, n, out, &l);
}
#else
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
static inline void meshSha256(const uint8_t* d, size_t n, uint8_t out[32]) { mbedtls_sha256(d, n, out, 0); }
static inline void meshHmac(const uint8_t key[32], const uint8_t* d, size_t n, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, d, n, out);
}
#endif

static inline void meshNormGroup(const char* in, char* out, size_t cap) {
  size_t k = 0;
  for (const char* p = in; *p && k + 1 < cap; p++) out[k++] = (char)tolower((unsigned char)*p);
  out[k] = 0;
}
static inline uint32_t meshGroupCode(const char* group) {
  char g[32]; meshNormGroup(group, g, sizeof g);
  uint32_t h = 2166136261u;
  for (const char* p = g; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  return h ? h : 1;
}
static inline void meshKey(const char* group, const char* pin, uint8_t key[32]) {
  char g[32]; meshNormGroup(group, g, sizeof g);
  char buf[96];
  int n = 0;
  const char* pre = "CardioResp-mesh/v2|";
  for (const char* p = pre; *p && n < (int)sizeof buf - 1; p++) buf[n++] = *p;
  for (const char* p = g; *p && n < (int)sizeof buf - 1; p++) buf[n++] = *p;
  if (n < (int)sizeof buf - 1) buf[n++] = '|';
  for (const char* p = pin; *p && n < (int)sizeof buf - 1; p++) buf[n++] = *p;
  meshSha256((const uint8_t*)buf, (size_t)n, key);
}
// pkt: the whole packet; ttlOff/hopsOff: offsets of the hop fields; tagOff: offset of the 8-byte tag
static inline void meshTag(const uint8_t key[32], const uint8_t* pkt, size_t len, size_t ttlOff, size_t hopsOff, size_t tagOff, uint8_t tag[8]) {
  uint8_t tmp[128];
  if (len > sizeof tmp) { memset(tag, 0, 8); return; }
  memcpy(tmp, pkt, len);
  tmp[ttlOff] = 0; tmp[hopsOff] = 0; memset(tmp + tagOff, 0, 8);
  uint8_t mac[32]; meshHmac(key, tmp, len, mac);
  memcpy(tag, mac, 8);
}
static inline bool meshTagOk(const uint8_t key[32], const uint8_t* pkt, size_t len, size_t ttlOff, size_t hopsOff, size_t tagOff) {
  uint8_t t[8]; meshTag(key, pkt, len, ttlOff, hopsOff, tagOff, t);
  uint8_t d = 0; for (int i = 0; i < 8; i++) d |= (uint8_t)(t[i] ^ pkt[tagOff + i]);   // constant time
  return d == 0;
}
// PIN: empty (open mesh) or 4-8 digits
static inline bool meshPinValid(const char* pin) {
  size_t n = strlen(pin);
  if (n == 0) return true;
  if (n < 4 || n > 8) return false;
  for (size_t i = 0; i < n; i++) if (pin[i] < '0' || pin[i] > '9') return false;
  return true;
}
// group: 1-15 characters, letters, digits, space, - and _
static inline bool meshGroupValid(const char* g) {
  size_t n = strlen(g);
  if (n < 1 || n > 15) return false;
  for (size_t i = 0; i < n; i++) { char c = g[i]; if (!(isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')) return false; }
  return true;
}
