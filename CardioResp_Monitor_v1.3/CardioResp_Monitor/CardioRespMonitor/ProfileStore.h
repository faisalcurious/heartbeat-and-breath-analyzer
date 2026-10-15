// =====================================================================
//  ProfileStore.h - patient and nurse profiles kept in flash (NVS)
//  Patient and nurse are separate records edited on separate pages.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "Protocol.h"

struct PatientProfile {
  char name[32] = "", id[20] = "", sex[10] = "", bed[16] = "", place[28] = "", posture[24] = "", notes[120] = "";
};
struct NurseProfile {
  char name[32] = "", id[20] = "", ward[24] = "", phone[24] = "", shift[16] = "";
};

class ProfileStore {
 public:
  PatientProfile patient;
  NurseProfile nurse;
  void begin() {
    m_p.begin("cardioprof", false);
    load("p_name", patient.name, sizeof patient.name); load("p_id", patient.id, sizeof patient.id);
    load("p_sex", patient.sex, sizeof patient.sex); load("p_bed", patient.bed, sizeof patient.bed);
    load("p_place", patient.place, sizeof patient.place); load("p_post", patient.posture, sizeof patient.posture);
    load("p_notes", patient.notes, sizeof patient.notes);
    load("n_name", nurse.name, sizeof nurse.name); load("n_id", nurse.id, sizeof nurse.id);
    load("n_ward", nurse.ward, sizeof nurse.ward); load("n_phone", nurse.phone, sizeof nurse.phone);
    load("n_shift", nurse.shift, sizeof nurse.shift);
  }
  // kind: "patient" | "nurse"; fields come from the form body
  template <class Getter>
  bool save(const char* kind, Getter arg) {
    if (!strcmp(kind, "patient")) {
      put("p_name", patient.name, sizeof patient.name, arg("name")); put("p_id", patient.id, sizeof patient.id, arg("id"));
      put("p_sex", patient.sex, sizeof patient.sex, arg("sex")); put("p_bed", patient.bed, sizeof patient.bed, arg("bed"));
      put("p_place", patient.place, sizeof patient.place, arg("place")); put("p_post", patient.posture, sizeof patient.posture, arg("post"));
      put("p_notes", patient.notes, sizeof patient.notes, arg("notes"));
      return true;
    }
    if (!strcmp(kind, "nurse")) {
      put("n_name", nurse.name, sizeof nurse.name, arg("name")); put("n_id", nurse.id, sizeof nurse.id, arg("id"));
      put("n_ward", nurse.ward, sizeof nurse.ward, arg("ward")); put("n_phone", nurse.phone, sizeof nurse.phone, arg("phone"));
      put("n_shift", nurse.shift, sizeof nurse.shift, arg("shift"));
      return true;
    }
    return false;
  }
  void writeJson(JBuf& j) {
    j.raw("{\"patient\":{");
    j.ks("name", patient.name); j.ks("id", patient.id); j.ks("sex", patient.sex); j.ks("bed", patient.bed);
    j.ks("place", patient.place); j.ks("post", patient.posture); j.ks("notes", patient.notes);
    j.raw("},\"nurse\":{");
    j.ks("name", nurse.name); j.ks("id", nurse.id); j.ks("ward", nurse.ward); j.ks("phone", nurse.phone); j.ks("shift", nurse.shift);
    j.raw("}}");
  }
 private:
  Preferences m_p;
  void load(const char* k, char* dst, size_t n) { String s = m_p.getString(k, ""); strlcpy(dst, s.c_str(), n); }
  static void clean(char* dst, size_t n, const String& v) {   // printable text only, no quotes/backslashes (JSON safe)
    size_t o = 0;
    for (size_t i = 0; i < v.length() && o + 1 < n; i++) {
      char c = v[i];
      if ((unsigned char)c < 0x20 || c == '"' || c == '\\') c = ' ';
      dst[o++] = c;
    }
    dst[o] = 0;
  }
  void put(const char* k, char* dst, size_t n, const String& v) {
    clean(dst, n, v);
    // trim
    size_t l = strlen(dst); while (l && dst[l - 1] == ' ') dst[--l] = 0;
    m_p.putString(k, dst);
  }
};
