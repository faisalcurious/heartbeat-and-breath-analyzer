// =====================================================================
//  OtaUpdate.h - firmware update from the Settings page
//
//  Accepts BOTH file kinds the Arduino IDE / PlatformIO produce:
//    * the sketch image   (<sketch>.ino.bin / firmware.bin)
//    * the "merged" image (<sketch>.ino.merged.bin / firmware.factory.bin):
//      bootloader + partition table + boot_app0 + app. Only the app part
//      (from offset 0x10000) is written - bootloader and partition table
//      on the board stay as they are, so a bad file cannot break boot.
//  The first bytes are checked (image magic, chip = ESP32-C3, app descriptor)
//  before anything is written to the inactive OTA slot; the running firmware
//  is only replaced after the whole image verified (Update.end).
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Update.h>

#define OTA_MERGED_APP_OFFSET 0x10000u

class OtaUpdate {
 public:
  bool busy() const { return m_started; }
  bool done() const { return m_done; }
  bool failed() const { return m_err[0] != 0; }
  const char* error() const { return m_err; }
  uint32_t bytes() const { return m_written; }
  const char* kind() const { return m_merged ? "merged image (app part used)" : "sketch image"; }

  void begin() { reset(); m_active = true; }
  // feed upload data in order
  void write(const uint8_t* d, size_t n) {
    if (!m_active || m_err[0]) return;
    m_total += n;
    while (n) {
      if (m_skip) {                                   // inside bootloader / partition table part of a merged file
        size_t k = m_skip < n ? m_skip : n;
        // remember the first 40 bytes to look at the app header after the skipped part
        d += k; n -= k; m_skip -= (uint32_t)k;
        if (m_skip) return;
        continue;
      }
      if (!m_started) {                               // collect the 40 header bytes
        size_t k = (m_hdrN + n > sizeof m_hdr) ? sizeof m_hdr - m_hdrN : n;
        memcpy(m_hdr + m_hdrN, d, k); m_hdrN += k; d += k; n -= k;
        if (m_hdrN < sizeof m_hdr) return;
        if (!decide()) return;
        if (m_skip) { m_hdrN = 0; continue; }         // merged: drop what we collected, skip to the app, collect again
        if (!startUpdate()) return;
        if (Update.write(m_hdr, m_hdrN) != m_hdrN) { fail("Flash write failed"); return; }
        m_written += m_hdrN;
        continue;
      }
      size_t w = Update.write((uint8_t*)d, n);
      if (w != n) { fail(Update.errorString()); return; }
      m_written += (uint32_t)w; d += w; n -= w;
    }
  }
  bool finish() {
    if (!m_active) return false;
    m_active = false;
    if (m_err[0]) { if (m_started) Update.abort(); return false; }
    if (!m_started) { fail(m_total < 64 ? "Empty file" : "No application image found in this file (is it the bootloader or a partition table only?)"); return false; }
    if (!Update.end(true)) { fail(Update.errorString()); return false; }
    m_done = true;
    return true;
  }
  void reject(const char* m) { m_active = true; fail(m); }
  void abort() { if (m_started) Update.abort(); m_active = false; m_started = false; }

 private:
  void reset() { m_active = m_started = m_done = m_merged = false; m_hdrN = 0; m_skip = 0; m_written = 0; m_total = 0; m_err[0] = 0; m_sawMerged = false; }
  void fail(const char* m) { strlcpy(m_err, m, sizeof m_err); if (m_started) { Update.abort(); } m_started = false; }
  // true = continue. App image: byte0 0xE9, chip id (bytes 12-13) = ESP32-C3 (5), app descriptor magic 0xABCD5432 at 32.
  bool decide() {
    if (m_hdr[0] != 0xE9) { fail("This is not a CardioResp controller firmware file (wrong first byte)"); return false; }
    const bool appDesc = m_hdr[32] == 0x32 && m_hdr[33] == 0x54 && m_hdr[34] == 0xCD && m_hdr[35] == 0xAB;
    if (appDesc) {
      const uint16_t chip = m_hdr[12] | (m_hdr[13] << 8);
      if (chip != 5) { fail("This firmware file is for a different controller type"); return false; }
      return true;
    }
    if (!m_sawMerged) {                               // looks like a bootloader: treat as merged image
      m_sawMerged = true; m_merged = true; m_skip = OTA_MERGED_APP_OFFSET - (uint32_t)sizeof m_hdr;
      return true;
    }
    fail("No application image at offset 0x10000 of this merged file"); return false;
  }
  bool startUpdate() {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { fail(Update.errorString()); return false; }
    m_started = true; return true;
  }
  bool m_active = false, m_started = false, m_done = false, m_merged = false, m_sawMerged = false;
  uint8_t m_hdr[40]; size_t m_hdrN = 0;
  uint32_t m_skip = 0, m_written = 0, m_total = 0;
  char m_err[96] = "";
};
