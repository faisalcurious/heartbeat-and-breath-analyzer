// =====================================================================
//  MeshNode.h - ESP-NOW "mesh": every monitor broadcasts a compact status
//  packet; every monitor listens and keeps a table of all patients it
//  hears. Packets carry a hop counter (flood relay with duplicate
//  suppression), so a node out of range of the phone's monitor is still
//  heard through its neighbours.
//
//  v1.2 final: packets are signed with a key made from the GROUP name and
//  the PIN (MeshAuth.h). Data is only accepted from monitors with the same
//  group and PIN; other monitors heard nearby are listed separately as
//  "other group", "wrong PIN" or "older firmware" (verification list).
//
//  "Update all" broadcasts a refresh request; "Verify" broadcasts a verify
//  request that every nearby monitor of the same group answers at once
//  (answers are staggered by a few tens of ms so they do not collide).
//
//  Radio cost is tiny (76 bytes every interval); ESP-NOW needs no router,
//  but all monitors must be on the same Wi-Fi channel: the direct networks
//  all use AP_CHANNEL; devices joined to one router share its channel.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "Protocol.h"
#include "config.h"

#ifndef MESH_GROUP_DEFAULT
#define MESH_GROUP_DEFAULT "ward"            // older config.h files do not define these
#endif
#ifndef MESH_PIN_DEFAULT
#define MESH_PIN_DEFAULT   ""
#endif

#pragma pack(push, 1)
struct MeshPkt {
  uint8_t magic0, magic1, ver, type;      // 'C','R',2; 1 = status, 2 = refresh request, 3 = verify request
  uint8_t ttl, hops; uint16_t seq;
  uint8_t src[6];
  uint32_t group;                         // group code (MeshAuth.h)
  char name[16];                          // patient name (or the monitor id)
  char label[16];                         // monitor label, e.g. bed / room
  uint16_t hr10, rr10;
  uint8_t sqh, sqr, sev, rel, mlc, mls, flags, ageY;
  int16_t t100;                           // body temperature degC * 100, -32768 = unknown
  uint32_t upS;
  uint8_t tag[8];                         // signature (MeshAuth.h)
};
#pragma pack(pop)
#define MESH_PKT_V1_LEN 48                // v1.1 / early v1.2 packets (unsigned)

#define MESH_F_PRES   0x01
#define MESH_F_MOTION 0x02
#define MESH_F_FULL   0x04
#define MESH_F_SENSOR 0x08
#define MESH_F_CAL    0x10

#define MESH_NEAR_GROUP 1                 // heard, but another group
#define MESH_NEAR_PIN   2                 // same group, wrong PIN
#define MESH_NEAR_OLD   3                 // older firmware without PIN support

struct MeshCfg {
  char grp[16] = MESH_GROUP_DEFAULT;
  char pin[9] = MESH_PIN_DEFAULT;
  char lbl[16] = "";
  uint16_t periodMs = MESH_PERIOD_MS;
  uint8_t ttl = MESH_TTL;
};

struct MeshNodeInfo {
  bool used = false;
  uint8_t mac[6];
  MeshPkt p;
  int8_t rssi = 0;
  uint32_t lastMs = 0;
};
struct MeshNear {
  bool used = false;
  uint8_t mac[6];
  char name[17] = "", label[17] = "";
  uint8_t st = 0;
  int8_t rssi = 0;
  uint32_t lastMs = 0;
};

class MeshNode {
 public:
  void begin(bool enabled);
  void loop();                               // call from the main loop (cheap)
  void setEnabled(bool on);
  bool enabled() const { return m_enabled; }
  bool up() const { return m_up; }
  void configure(const MeshCfg& c);          // group, PIN, label, interval, relay hops (clears the tables)
  const MeshCfg& cfg() const { return m_cfg; }
  void setSelf(const MeshPkt& p, const char* id6);   // newest local data (fields except src/ttl/seq/magic/tag)
  void requestAll();                         // broadcast a refresh request now
  void verifyAll();                          // broadcast a verify request now (fills the verification list)
  void writeJson(JBuf& j);
  int count() const;
  uint32_t version() const { return m_ver; } // increments whenever the table changes
  // called from the ESP-NOW receive callback (WiFi task)
  void onRx(const uint8_t* src, const uint8_t* data, int len, int rssi);

 private:
  bool ensure();
  void sign(MeshPkt& p);
  void send(MeshPkt& p);
  void sendStatus(uint8_t type = 1);
  void sendRequest(uint8_t type);
  void handle(const uint8_t* b, int len, int rssi);
  void noteNear(const uint8_t* mac, const char* name, const char* label, uint8_t st, int rssi);
  void dropNear(const uint8_t* mac);
  bool seen(const uint8_t* src, uint16_t seq);
  MeshNodeInfo* slot(const uint8_t* mac);
  bool m_enabled = false, m_up = false;
  uint8_t m_mode = 0;
  uint8_t m_mac[6];
  MeshCfg m_cfg;
  uint32_t m_group = 0;
  uint8_t m_key[32];
  MeshPkt m_self;
  bool m_haveSelf = false;
  char m_id[8] = "";
  uint16_t m_seq = 0;
  uint32_t m_lastTx = 0, m_lastEnsure = 0, m_ver = 0, m_replyAt = 0, m_verifyAt = 0;
  MeshNodeInfo m_nodes[MESH_MAX_NODES];
  MeshNear m_near[8];
  struct Seen { uint32_t key; uint16_t seq; } m_seen[32];
  int m_seenIdx = 0;
  struct Rx { uint8_t b[sizeof(MeshPkt)]; uint8_t len; int8_t rssi; } m_q[8];
  volatile uint8_t m_qh = 0, m_qt = 0;
};
