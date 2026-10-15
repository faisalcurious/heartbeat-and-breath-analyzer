// =====================================================================
//  name_test.cpp - v1.3 adaptive local names (NetNames.h) on a simulated network of monitors.
//  build: g++ -O2 -std=c++17 -ICardioRespMonitor host_test/name_test.cpp -o name_test && ./name_test
//  The "network" answers a name query with the addresses of the monitors announcing that name,
//  exactly what the mDNS query on the controller returns; the decision code is the firmware's own.
// =====================================================================
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "NetNames.h"

static int fails = 0;
static void check(const char* what, bool ok) { printf("  %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }
static uint32_t ip(int last) { uint8_t b[4] = {192, 168, 1, (uint8_t)last}; uint32_t v; memcpy(&v, b, 4); return v; }   // network order

struct Monitor { std::string unique, saved, name; uint32_t addr; bool online = false; };
struct Net {
  std::vector<Monitor*> mons;
  std::vector<uint32_t> query(const std::string& n) {          // addresses answering for n
    std::vector<uint32_t> a;
    for (auto* m : mons) if (m->online && m->name == n) a.push_back(m->addr);
    return a;
  }
};
// same steps as NetManager::nameLoop: candidates from the saved / current name, first free one is taken
static void search(Net& net, Monitor& m) {
  const std::string first = m.name.size() && m.name != m.unique ? m.name : m.saved;
  for (int k = 0;; k++) {
    char c[32];
    if (!nameCandidate(k, first.empty() ? nullptr : first.c_str(), c, sizeof c)) { m.name = m.unique; return; }
    auto ans = net.query(c);
    const bool mine = m.name == c;
    if (!nameTaken(ans.data(), (int)ans.size(), m.addr, mine)) { m.name = c; m.saved = c; return; }
  }
}
static void boot(Net& net, Monitor& m) { m.online = true; m.name = m.unique; search(net, m); }   // boots with its unique name, then checks

int main() {
  printf("candidate order and name rules\n");
  char c[32]; std::string seq;
  for (int k = 0; nameCandidate(k, nullptr, c, sizeof c); k++) seq += std::string(c) + " ";
  check("no saved name: cardio cardio2 ... cardio9", seq == "cardio cardio2 cardio3 cardio4 cardio5 cardio6 cardio7 cardio8 cardio9 ");
  seq.clear();
  for (int k = 0; nameCandidate(k, "cardio3", c, sizeof c); k++) seq += std::string(c) + " ";
  check("saved name first, no repeat", seq == "cardio3 cardio cardio2 cardio4 cardio5 cardio6 cardio7 cardio8 cardio9 ");
  check("valid names", validName("cardio2") && validName("bed-4") && !validName("Bed4") && !validName("") && !validName("a b"));

  printf("monitors joining one Wi-Fi network\n");
  Net net;
  std::vector<Monitor> M(11);
  for (int i = 0; i < 11; i++) { char u[32]; snprintf(u, sizeof u, "cardio-a1b2%02d", i); M[i].unique = u; M[i].addr = ip(20 + i); }
  for (auto& m : M) net.mons.push_back(&m);
  boot(net, M[0]); boot(net, M[1]); boot(net, M[2]);
  printf("    %s %s %s\n", M[0].name.c_str(), M[1].name.c_str(), M[2].name.c_str());
  check("first three get cardio, cardio2, cardio3", M[0].name == "cardio" && M[1].name == "cardio2" && M[2].name == "cardio3");
  M[1].online = false; boot(net, M[1]);
  check("a restarted monitor keeps its name", M[1].name == "cardio2");
  for (int i = 3; i < 9; i++) boot(net, M[i]);
  check("nine monitors: cardio ... cardio9", M[8].name == "cardio9");
  boot(net, M[9]);
  check("tenth monitor uses its unique name", M[9].name == M[9].unique);
  bool unique = true;
  for (int i = 0; i < 10; i++) for (int j = i + 1; j < 10; j++) if (M[i].name == M[j].name) unique = false;
  check("all names different", unique);

  printf("clash after the network was split (both call themselves cardio)\n");
  Net n2;
  Monitor A{"cardio-aaaaaa", "cardio", "cardio", ip(10), true}, B{"cardio-bbbbbb", "cardio", "cardio", ip(30), true};
  n2.mons = {&A, &B};
  search(n2, B); search(n2, A);                  // the periodic re-check runs on both, in any order
  printf("    A(.10) %s  B(.30) %s\n", A.name.c_str(), B.name.c_str());
  check("lower address keeps the name, the other moves on", A.name == "cardio" && B.name == "cardio2");
  search(n2, A); search(n2, B);
  check("stable afterwards (no ping-pong)", A.name == "cardio" && B.name == "cardio2");
  printf("%s (%d failed)\n", fails ? "SOME NAME CHECKS FAILED" : "ALL NAME CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
