// =====================================================================
//  NetNames.h  -  v1.3 adaptive local names (pure C++, also compiled on the PC: host_test/name_test.cpp)
//
//  The first monitor on a network is http://cardio.local, the next cardio2.local ... cardio9.local.
//  A monitor first tries the name it had before (kept in flash), so names stay the same across restarts.
//  A candidate is free when no OTHER address answers for it. If two monitors ever end up with the same
//  name, the one with the lower IP address keeps it and the other moves on (no ping-pong).
// =====================================================================
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef NAME_BASE
#define NAME_BASE "cardio"
#endif

// candidate k: 'first' (the current / saved name, if any), then cardio, cardio2 ... cardio9 (without repeating 'first')
inline bool nameCandidate(int k, const char* first, char* out, size_t n) {
  if (k < 0) return false;
  if (first && first[0]) { if (k == 0) { snprintf(out, n, "%s", first); return true; } k--; }
  for (int i = 0; i < 9; i++) {
    char c[32];
    if (i == 0) snprintf(c, sizeof c, "%s", NAME_BASE); else snprintf(c, sizeof c, "%s%d", NAME_BASE, i + 1);
    if (first && !strcmp(c, first)) continue;
    if (k-- == 0) { snprintf(out, n, "%s", c); return true; }
  }
  return false;
}

inline bool validName(const char* s) {
  if (!s || !*s || strlen(s) > 31) return false;
  for (const char* p = s; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
  return true;
}

// IPv4 addresses in network byte order (as stored by the network stack); host order is used for the comparison
inline uint32_t ipHostOrder(uint32_t netOrder) {
  const uint8_t* b = (const uint8_t*)&netOrder;
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// Is the name taken? answers = addresses that answered for it; me = this monitor's address; mine = this
// monitor is using the name right now (then a clash is settled by the lower address).
inline bool nameTaken(const uint32_t* answers, int n, uint32_t me, bool mine) {
  for (int i = 0; i < n; i++) {
    const uint32_t other = answers[i];
    if (!other || other == me) continue;
    if (!mine || ipHostOrder(other) < ipHostOrder(me)) return true;
  }
  return false;
}
