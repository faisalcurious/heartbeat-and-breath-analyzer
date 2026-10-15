// PC test of the mesh packet signature (MeshAuth.h) - build:
//   g++ -std=c++17 -DMESH_AUTH_OPENSSL -ICardioRespMonitor host_test/mesh_auth_test.cpp -lcrypto -o mesh_auth_test
#include <cstdio>
#include <cstddef>
#include "MeshAuth.h"
#pragma pack(push, 1)
struct Pkt {                       // same layout as MeshPkt in MeshNode.h
  uint8_t magic0, magic1, ver, type; uint8_t ttl, hops; uint16_t seq; uint8_t src[6]; uint32_t group;
  char name[16]; char label[16]; uint16_t hr10, rr10; uint8_t sqh, sqr, sev, rel, mlc, mls, flags, ageY; int16_t t100; uint32_t upS; uint8_t tag[8];
};
#pragma pack(pop)
static int fails = 0;
static void chk(const char* n, bool c) { printf("%s %s\n", c ? "PASS" : "FAIL", n); if (!c) fails++; }
static const size_t T = offsetof(Pkt, ttl), H = offsetof(Pkt, hops), G = offsetof(Pkt, tag);
static Pkt make(const char* grp, const char* pin) {
  Pkt p; memset(&p, 0, sizeof p);
  p.magic0 = 'C'; p.magic1 = 'R'; p.ver = 2; p.type = 1; p.ttl = 2; p.seq = 7;
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xA1, 0xB2, 0xC3}; memcpy(p.src, mac, 6);
  strcpy(p.name, "A. Patient"); strcpy(p.label, "Bed 4"); p.hr10 = 725; p.rr10 = 160; p.sev = 0; p.t100 = 3680; p.upS = 1234;
  p.group = meshGroupCode(grp);
  uint8_t k[32]; meshKey(grp, pin, k); meshTag(k, (uint8_t*)&p, sizeof p, T, H, G, p.tag);
  return p;
}
static bool ok(const Pkt& p, const char* grp, const char* pin) {
  uint8_t k[32]; meshKey(grp, pin, k);
  return p.group == meshGroupCode(grp) && meshTagOk(k, (const uint8_t*)&p, sizeof p, T, H, G);
}
int main() {
  printf("packet size %zu (ESP-NOW limit 250)\n", sizeof(Pkt));
  chk("packet fits ESP-NOW", sizeof(Pkt) <= 250);
  Pkt p = make("Ward 3", "4821");
  chk("same group and PIN verifies", ok(p, "Ward 3", "4821"));
  chk("group name is not case sensitive", ok(p, "ward 3", "4821"));
  chk("wrong PIN rejected", !ok(p, "Ward 3", "4822"));
  chk("other group rejected", !ok(p, "Ward 4", "4821"));
  chk("open mesh (no PIN) rejects a PIN-protected packet", !ok(p, "Ward 3", ""));
  Pkt o = make("Ward 3", "");
  chk("open mesh packets verify in an open mesh", ok(o, "Ward 3", ""));
  chk("open mesh packet rejected when a PIN is set", !ok(o, "Ward 3", "4821"));
  Pkt r = p; r.ttl = 1; r.hops = 1;
  chk("relayed packet (ttl/hops changed) still verifies", ok(r, "Ward 3", "4821"));
  Pkt t = p; t.hr10 = 1325;
  chk("changed heart rate is detected", !ok(t, "Ward 3", "4821"));
  Pkt n = p; n.name[0] = 'B';
  chk("changed name is detected", !ok(n, "Ward 3", "4821"));
  chk("PIN rules", meshPinValid("") && meshPinValid("1234") && meshPinValid("12345678") && !meshPinValid("123") && !meshPinValid("123456789") && !meshPinValid("12a4"));
  chk("group rules", meshGroupValid("Ward 3") && meshGroupValid("icu_2-b") && !meshGroupValid("") && !meshGroupValid("this name is too long") && !meshGroupValid("a/b"));
  // print a vector for the independent Python check
  printf("VECTOR group=%08x tag=", p.group); for (int i = 0; i < 8; i++) printf("%02x", p.tag[i]); printf(" bytes=");
  for (size_t i = 0; i < sizeof p; i++) printf("%02x", ((uint8_t*)&p)[i]); printf("\n");
  printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
  return fails ? 1 : 0;
}
