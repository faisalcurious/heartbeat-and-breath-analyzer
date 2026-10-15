// =====================================================================
//  Recorder.h - record a short measurement (default 60 s) into flash
//
//  File = sequence of chunks  [tag u8][len u16][payload]   (little endian)
//    'H' header   RecHeader
//    'V' vitals   RecRow     once per second
//    'D' waveform int16 {scg, gcg} pairs at 100 Hz (0.01 mg, 0.001 dps)   - licensed units only
//    'R' breathing int16 at 10 Hz (mdeg)                                  - licensed units only
//    'E' end      u16 seconds recorded
//  v1.4 (header ver 2): waveforms are stored compact as 'd' / 'r' (zig-zag varint deltas, about 2-3x smaller)
//  and every byte after the 'H' chunk is encrypted with a keystream (REC_FILE_KEY + per-file nonce).
//  The dashboard decrypts and converts files to CSV (and a ZIP of all files); ver 1 files are still read.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "Protocol.h"
#include "config.h"

#ifndef REC_FILE_KEY
#define REC_FILE_KEY 0x5A17C0DEu   // must match REC_KEY in web/ext.js
#endif

#pragma pack(push, 1)
struct RecHeader {
  uint32_t epoch;        // 0 = clock unknown
  uint32_t upMs;
  uint16_t seconds;      // planned
  float age;
  char dev[12];
  char name[24];
  char pid[16];
  uint8_t full;          // 1 = waveforms included
  uint8_t ver;           // 1
};
struct RecRow { uint16_t hr10, rr10; uint8_t sqh, sqr, sev, flags; int16_t tc100; uint16_t rmssd10; };
#pragma pack(pop)

class Recorder {
 public:
  bool begin();
  bool ready() const { return m_fsOk; }
  bool start(VitalsEngine& e, int seconds, const RecHeader& h, char* err, size_t errLen);
  void stop(bool completed);
  // call once per second while measuring; returns true when the recording finished by itself
  bool tick(VitalsEngine& e, const RecRow& row);
  bool active() const { return m_active; }
  int elapsed() const { return m_sec; }
  int total() const { return m_total; }
  const char* currentName() const { return m_name; }
  void listJson(JBuf& j);
  bool openRead(const char* name, File& f);
  bool remove(const char* name);
  int removeAll();
  size_t freeBytes() const;
  size_t totalBytes() const;
 private:
  bool nameOk(const char* n) const;
  bool m_fsOk = false, m_active = false, m_full = false;
  int m_sec = 0, m_total = 0;
  char m_name[24] = "";
  File m_f;
  uint32_t m_cd = 0, m_cr = 0;     // cursors into the engine rings
  uint32_t m_sid = 0;
  uint32_t m_ks = 0;               // keystream state (0 = plain, header only)
  void chunk(char tag, const void* data, uint16_t len);
  void put(const uint8_t* p, size_t n);
};
