#include "Recorder.h"

bool Recorder::begin() {
  m_fsOk = LittleFS.begin(true, "/littlefs", 10, "spiffs");     // formats on first use
  if (m_fsOk) Serial.printf("[REC] flash storage %u kB, free %u kB\n", (unsigned)(totalBytes() / 1024), (unsigned)(freeBytes() / 1024));
  else Serial.println(F("[REC] flash storage not available (partition table without a data partition?)"));
  return m_fsOk;
}
size_t Recorder::totalBytes() const { return m_fsOk ? LittleFS.totalBytes() : 0; }
size_t Recorder::freeBytes() const { return m_fsOk ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0; }

bool Recorder::nameOk(const char* n) const {
  if (!n || strncmp(n, "rec", 3) || strlen(n) > 20) return false;
  for (const char* p = n; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_')) return false;
  return true;
}

// v1.4: everything after the header is XORed with a xorshift32 keystream
void Recorder::put(const uint8_t* p, size_t n) {
  if (!m_ks) { m_f.write(p, n); return; }
  uint8_t t[64];
  while (n) {
    const size_t k = n < sizeof t ? n : sizeof t;
    for (size_t i = 0; i < k; i++) { m_ks ^= m_ks << 13; m_ks ^= m_ks >> 17; m_ks ^= m_ks << 5; t[i] = p[i] ^ (uint8_t)m_ks; }
    m_f.write(t, k); p += k; n -= k;
  }
}

void Recorder::chunk(char tag, const void* data, uint16_t len) {
  uint8_t h[3] = {(uint8_t)tag, (uint8_t)(len & 255), (uint8_t)(len >> 8)};
  put(h, 3);
  if (len) put((const uint8_t*)data, len);
}

// zig-zag varint of a delta: small changes take 1 byte instead of 2
static inline uint8_t* putVar(uint8_t* o, int32_t d) {
  uint32_t z = ((uint32_t)d << 1) ^ (uint32_t)(d >> 31);
  while (z >= 0x80) { *o++ = (uint8_t)(z | 0x80); z >>= 7; }
  *o++ = (uint8_t)z;
  return o;
}

bool Recorder::start(VitalsEngine& e, int seconds, const RecHeader& hdr, char* err, size_t errLen) {
  if (!m_fsOk) { snprintf(err, errLen, "Flash storage is not available"); return false; }
  if (m_active) { snprintf(err, errLen, "Already recording"); return false; }
  if (seconds < 5) seconds = 5;
  if (seconds > REC_MAX_SECONDS) seconds = REC_MAX_SECONDS;
  const size_t need = (size_t)seconds * (hdr.full ? 560 : 40) + 400;
  if (freeBytes() < need + REC_KEEP_FREE_BYTES) { snprintf(err, errLen, "Not enough memory left (%u kB free). Delete old recordings first.", (unsigned)(freeBytes() / 1024)); return false; }
  Preferences p; p.begin("cardiorec", false);
  uint32_t n = p.getUInt("n", 0) + 1; p.putUInt("n", n); p.end();
  snprintf(m_name, sizeof m_name, "rec%04u.crr", (unsigned)(n % 10000));
  char path[32]; snprintf(path, sizeof path, "/%s", m_name);
  m_f = LittleFS.open(path, "w");
  if (!m_f) { snprintf(err, errLen, "Cannot create the file"); return false; }
  RecHeader h = hdr; h.seconds = (uint16_t)seconds; h.ver = 2;
  m_ks = 0;
  chunk('H', &h, sizeof h);                                      // header stays readable (file list, name)
  m_ks = (REC_FILE_KEY ^ h.epoch ^ (h.upMs * 2654435761u)) | 1u;  // must match recKs() in web/ext.js
  m_full = hdr.full != 0;
  m_cd = e.disp.head; m_cr = e.resp.head; m_sid = e.session();
  m_sec = 0; m_total = seconds; m_active = true;
  err[0] = 0;
  return true;
}

void Recorder::stop(bool completed) {
  if (!m_active) return;
  uint16_t s = (uint16_t)m_sec;
  chunk('E', &s, 2);
  m_f.close();
  m_active = false;
  if (!completed && m_sec < 3) { char path[32]; snprintf(path, sizeof path, "/%s", m_name); LittleFS.remove(path); }
}

bool Recorder::tick(VitalsEngine& e, const RecRow& row) {
  if (!m_active) return false;
  if (e.session() != m_sid) { stop(false); return true; }          // measurement was restarted / recalibrated
  chunk('V', &row, sizeof row);
  if (m_full) {
    uint32_t dh = e.disp.head;
    if (m_cd + VE_DISP_LEN - 64 < dh) m_cd = dh - (VE_DISP_LEN - 64);
    static uint8_t buf[64 * 2 * 3];                               // 'd': deltas restart from 0 in every chunk
    while (m_cd < dh) {
      int n = 0; int32_t pa = 0, pg = 0; uint8_t* o = buf;
      while (m_cd < dh && n < 64) { const DispSample& d = e.disp.at(m_cd++); o = putVar(o, d.scg - pa); o = putVar(o, d.gcg - pg); pa = d.scg; pg = d.gcg; n++; }
      chunk('d', buf, (uint16_t)(o - buf));
    }
    uint32_t rh = e.resp.head;
    if (m_cr + VE_RESP_LEN - 64 < rh) m_cr = rh - (VE_RESP_LEN - 64);
    static uint8_t rb[64 * 3];
    while (m_cr < rh) {
      int n = 0; int32_t pr = 0; uint8_t* o = rb;
      while (m_cr < rh && n < 64) { const int16_t v = e.resp.at(m_cr++); o = putVar(o, v - pr); pr = v; n++; }
      chunk('r', rb, (uint16_t)(o - rb));
    }
  }
  if ((m_sec & 7) == 7) m_f.flush();                               // fewer flash writes (close() flushes the rest)
  m_sec++;
  if (m_sec >= m_total) { stop(true); return true; }
  return false;
}

void Recorder::listJson(JBuf& j) {
  j.raw("{\"ok\":"); j.raw(m_fsOk ? "1" : "0");
  j.ki("free", (long)freeBytes()); j.ki("total", (long)totalBytes());
  j.ki("rec", m_active ? 1 : 0); j.ki("sec", m_sec); j.ki("tot", m_total); j.ks("cur", m_active ? m_name : "");
  j.key("files"); j.raw("[");
  if (m_fsOk) {
    File root = LittleFS.open("/");
    bool first = true;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (f.isDirectory()) continue;
      const char* nm = f.name(); if (*nm == '/') nm++;
      if (strncmp(nm, "rec", 3) || j.nearlyFull(120)) continue;
      if (!first) j.raw(",");
      first = false;
      j.raw("{"); j.ks("n", nm); j.ki("s", (long)f.size()); j.raw("}");
    }
  }
  j.raw("]}");
}

bool Recorder::openRead(const char* name, File& f) {
  if (!m_fsOk || !nameOk(name)) return false;
  char path[32]; snprintf(path, sizeof path, "/%s", name);
  f = LittleFS.open(path, "r");
  return (bool)f;
}
bool Recorder::remove(const char* name) {
  if (!m_fsOk || !nameOk(name) || (m_active && !strcmp(name, m_name))) return false;
  char path[32]; snprintf(path, sizeof path, "/%s", name);
  return LittleFS.remove(path);
}
int Recorder::removeAll() {
  if (!m_fsOk) return 0;
  String names[24]; int n = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f && n < 24; f = root.openNextFile()) { const char* nm = f.name(); if (*nm == '/') nm++; if (!f.isDirectory() && !strncmp(nm, "rec", 3)) names[n++] = nm; }
  int c = 0;
  for (int i = 0; i < n; i++) if (remove(names[i].c_str())) c++;
  return c;
}
