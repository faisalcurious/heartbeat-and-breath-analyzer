#include "MeshNode.h"
#include "MeshAuth.h"
#include <stddef.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

static MeshNode* s_mesh = nullptr;
static portMUX_TYPE s_meshMux = portMUX_INITIALIZER_UNLOCKED;
static const uint8_t kBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const size_t kTtlOff = offsetof(MeshPkt, ttl), kHopsOff = offsetof(MeshPkt, hops), kTagOff = offsetof(MeshPkt, tag);

static void onRecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (s_mesh && info) s_mesh->onRx(info->src_addr, data, len, info->rx_ctrl ? info->rx_ctrl->rssi : 0);
}

void MeshNode::begin(bool enabled) {
  s_mesh = this;
  esp_read_mac(m_mac, ESP_MAC_WIFI_STA);
  m_enabled = enabled;
  memset(&m_self, 0, sizeof m_self);
  m_self.magic0 = 'C'; m_self.magic1 = 'R'; m_self.ver = 2;
  memcpy(m_self.src, m_mac, 6);
  m_self.t100 = INT16_MIN;
  memset(m_seen, 0, sizeof m_seen);
  configure(m_cfg);
}

void MeshNode::configure(const MeshCfg& c) {
  m_cfg = c;
  if (!meshGroupValid(m_cfg.grp)) strlcpy(m_cfg.grp, MESH_GROUP_DEFAULT, sizeof m_cfg.grp);
  if (!meshPinValid(m_cfg.pin)) m_cfg.pin[0] = 0;
  if (m_cfg.periodMs < 1000) m_cfg.periodMs = 1000;
  if (m_cfg.periodMs > 10000) m_cfg.periodMs = 10000;
  if (m_cfg.ttl > 3) m_cfg.ttl = 3;
  m_group = meshGroupCode(m_cfg.grp);
  meshKey(m_cfg.grp, m_cfg.pin, m_key);
  for (auto& n : m_nodes) n.used = false;              // a new group / PIN starts a fresh table
  for (auto& n : m_near) n.used = false;
  m_ver++;
}

void MeshNode::setEnabled(bool on) {
  m_enabled = on;
  if (!on && m_up) { esp_now_deinit(); m_up = false; }
  m_ver++;
}

bool MeshNode::ensure() {
  if (!m_enabled) return false;
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) { m_up = false; return false; }
  if (m_up && (uint8_t)mode == m_mode) return true;
  if (m_up) esp_now_deinit();
  m_up = false;
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onRecvCb);
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof peer);
  memset(peer.peer_addr, 0xFF, 6);
  peer.channel = 0;                                   // current channel
  peer.ifidx = (mode & WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) { esp_now_deinit(); return false; }
  m_mode = (uint8_t)mode; m_up = true;
  return true;
}

void MeshNode::setSelf(const MeshPkt& p, const char* id6) {
  uint16_t seq = m_self.seq;
  m_self = p;
  m_self.magic0 = 'C'; m_self.magic1 = 'R'; m_self.ver = 2; memcpy(m_self.src, m_mac, 6);
  m_self.seq = seq; m_self.group = m_group;
  strlcpy(m_id, id6, sizeof m_id);
  m_haveSelf = true;
}

void MeshNode::sign(MeshPkt& p) {
  p.group = m_group;
  meshTag(m_key, (const uint8_t*)&p, sizeof p, kTtlOff, kHopsOff, kTagOff, p.tag);
}
void MeshNode::send(MeshPkt& p) {
  sign(p);
  esp_now_send(kBcast, (const uint8_t*)&p, sizeof p);
  m_lastTx = millis();
}

void MeshNode::sendStatus(uint8_t type) {
  if (!m_up || !m_haveSelf) return;
  MeshPkt p = m_self;
  p.type = type; p.ttl = m_cfg.ttl; p.hops = 0; p.seq = ++m_seq;
  send(p);
}

void MeshNode::sendRequest(uint8_t type) {
  if (!ensure()) return;
  MeshPkt p; memset(&p, 0, sizeof p);
  p.magic0 = 'C'; p.magic1 = 'R'; p.ver = 2; p.type = type; p.ttl = m_cfg.ttl; p.hops = 0; p.seq = ++m_seq;
  memcpy(p.src, m_mac, 6);
  if (m_haveSelf) { memcpy(p.name, m_self.name, sizeof p.name); memcpy(p.label, m_self.label, sizeof p.label); }
  send(p);
  m_replyAt = millis() + 5;                          // answer our own request too (fresh data for the table)
}
void MeshNode::requestAll() { sendRequest(2); }
void MeshNode::verifyAll() {
  for (auto& n : m_near) n.used = false;             // a fresh verification list
  m_ver++;
  sendRequest(3);
}

void MeshNode::onRx(const uint8_t* src, const uint8_t* data, int len, int rssi) {
  (void)src;
  if (len < 4 || len > (int)sizeof(MeshPkt) || data[0] != 'C' || data[1] != 'R') return;
  portENTER_CRITICAL(&s_meshMux);
  uint8_t nh = (m_qh + 1) & 7;
  if (nh != m_qt) { memcpy(m_q[m_qh].b, data, len); m_q[m_qh].len = (uint8_t)len; m_q[m_qh].rssi = (int8_t)rssi; m_qh = nh; }
  portEXIT_CRITICAL(&s_meshMux);
}

bool MeshNode::seen(const uint8_t* src, uint16_t seq) {
  uint32_t key = ((uint32_t)src[2] << 24) | ((uint32_t)src[3] << 16) | ((uint32_t)src[4] << 8) | src[5];
  for (int i = 0; i < 32; i++) if (m_seen[i].key == key && m_seen[i].seq == seq) return true;
  m_seen[m_seenIdx].key = key; m_seen[m_seenIdx].seq = seq; m_seenIdx = (m_seenIdx + 1) & 31;
  return false;
}

MeshNodeInfo* MeshNode::slot(const uint8_t* mac) {
  MeshNodeInfo* freeS = nullptr; MeshNodeInfo* oldest = &m_nodes[0];
  for (auto& n : m_nodes) {
    if (n.used && !memcmp(n.mac, mac, 6)) return &n;
    if (!n.used && !freeS) freeS = &n;
    if (n.used && n.lastMs < oldest->lastMs) oldest = &n;
  }
  return freeS ? freeS : oldest;
}

void MeshNode::noteNear(const uint8_t* mac, const char* name, const char* label, uint8_t st, int rssi) {
  MeshNear* e = nullptr; MeshNear* oldest = &m_near[0];
  for (auto& n : m_near) {
    if (n.used && !memcmp(n.mac, mac, 6)) { e = &n; break; }
    if (!n.used && !e) e = &n;
    if (n.used && n.lastMs < oldest->lastMs) oldest = &n;
  }
  if (!e) e = oldest;
  const bool fresh = !e->used || e->st != st;
  e->used = true; memcpy(e->mac, mac, 6);
  memcpy(e->name, name, 16); e->name[16] = 0;
  if (label) { memcpy(e->label, label, 16); e->label[16] = 0; } else e->label[0] = 0;
  e->st = st; e->rssi = (int8_t)rssi; e->lastMs = millis();
  if (fresh) m_ver++;
}
void MeshNode::dropNear(const uint8_t* mac) {
  for (auto& n : m_near) if (n.used && !memcmp(n.mac, mac, 6)) { n.used = false; m_ver++; }
}

void MeshNode::handle(const uint8_t* b, int len, int rssi) {
  const uint8_t ver = b[2];
  if (ver == 1 && len == MESH_PKT_V1_LEN) {           // older firmware: listed, data not used
    if (memcmp(b + 8, m_mac, 6)) noteNear(b + 8, (const char*)(b + 14), nullptr, MESH_NEAR_OLD, rssi);
    return;
  }
  if (ver != 2 || len != (int)sizeof(MeshPkt)) return;
  MeshPkt p; memcpy(&p, b, sizeof p);
  if (!memcmp(p.src, m_mac, 6)) return;                // our own echo
  if (p.group != m_group) { noteNear(p.src, p.name, p.label, MESH_NEAR_GROUP, rssi); return; }
  if (!meshTagOk(m_key, b, sizeof p, kTtlOff, kHopsOff, kTagOff)) {
    noteNear(p.src, p.name, p.label, MESH_NEAR_PIN, rssi);
    // answer a verify request anyway (with our own signature), so the other monitor can list us as "wrong PIN"
    if (p.type == 3 && !m_replyAt) m_replyAt = millis() + (uint32_t)(m_mac[5] & 7) * 40u + 5u;
    return;                                            // never use or relay data we cannot verify
  }
  dropNear(p.src);
  if (seen(p.src, p.seq)) return;
  if (p.type == 1) {
    MeshNodeInfo* n = slot(p.src);
    n->used = true; memcpy(n->mac, p.src, 6); n->p = p; n->rssi = (int8_t)rssi; n->lastMs = millis();
    m_ver++;
  } else if (p.type == 2 || p.type == 3) {
    m_replyAt = millis() + (uint32_t)(m_mac[5] & 7) * 40u + 5u;   // staggered answer
  }
  if (p.ttl > 0 && m_cfg.ttl > 0) {                    // flood relay (the signature does not cover the hop fields)
    MeshPkt r = p; r.ttl--; r.hops++;
    delay(2 + (m_mac[5] & 7));                         // tiny jitter against collisions
    esp_now_send(kBcast, (const uint8_t*)&r, sizeof r);
  }
}

void MeshNode::loop() {
  const uint32_t now = millis();
  if (!m_enabled) return;
  if (now - m_lastEnsure > 3000 || !m_up) { m_lastEnsure = now; ensure(); }
  if (!m_up) return;
  while (m_qt != m_qh) {
    Rx r;
    portENTER_CRITICAL(&s_meshMux);
    r = m_q[m_qt]; m_qt = (m_qt + 1) & 7;
    portEXIT_CRITICAL(&s_meshMux);
    handle(r.b, r.len, r.rssi);
  }
  for (auto& n : m_nodes) if (n.used && now - n.lastMs > 300000u) { n.used = false; m_ver++; }   // gone for 5 min
  for (auto& n : m_near) if (n.used && now - n.lastMs > 300000u) { n.used = false; m_ver++; }
  if (m_replyAt && (int32_t)(now - m_replyAt) >= 0) { m_replyAt = 0; sendStatus(1); }
  const uint32_t period = m_cfg.periodMs + (m_mac[5] & 15) * 20u;     // spread the nodes in time
  if (now - m_lastTx >= period) sendStatus(1);
}

int MeshNode::count() const {
  int c = 0;
  for (auto& n : m_nodes) if (n.used) c++;
  return c;
}

void MeshNode::writeJson(JBuf& j) {
  const uint32_t now = millis();
  char me[8]; snprintf(me, sizeof me, "%02X%02X%02X", m_mac[3], m_mac[4], m_mac[5]);
  j.raw("{\"t\":\"m\"");
  j.ki("on", m_enabled ? 1 : 0); j.ki("up", m_up ? 1 : 0); j.ki("ttl", m_cfg.ttl); j.ki("ver", (long)m_ver);
  j.ks("grp", m_cfg.grp); j.ki("open", m_cfg.pin[0] ? 0 : 1); j.ks("lbl", m_cfg.lbl); j.ki("per", m_cfg.periodMs);
  j.ks("me", me); j.ki("ch", (long)WiFi.channel());
  j.key("nodes"); j.raw("[");
  bool first = true;
  auto emit = [&](const MeshPkt& p, const uint8_t* mac, int rssi, uint32_t ageMs, bool self) {
    if (j.nearlyFull(420)) return;
    if (!first) j.raw(",");
    first = false;
    char id[8]; snprintf(id, sizeof id, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    char nm[17]; memcpy(nm, p.name, 16); nm[16] = 0;
    char lb[17]; memcpy(lb, p.label, 16); lb[16] = 0;
    j.raw("{"); j.ks("id", id); j.ks("name", nm); j.ks("lbl", lb);
    j.kf("hr", p.hr10 ? p.hr10 / 10.0f : NAN, 1); j.kf("rr", p.rr10 ? p.rr10 / 10.0f : NAN, 1);
    j.ki("sqh", p.sqh); j.ki("sqr", p.sqr); j.ki("sev", p.sev); j.ki("rel", p.rel); j.ki("mlc", p.mlc); j.kf("mls", p.mls / 100.0f, 2);
    j.ki("fl", p.flags); j.ki("age", p.ageY);
    j.kf("t", p.t100 == INT16_MIN ? NAN : p.t100 / 100.0f, 1);
    j.ki("rssi", rssi); j.ki("hops", p.hops); j.ki("ago", (long)(ageMs / 1000)); j.ki("up", (long)p.upS); j.ki("self", self ? 1 : 0);
    j.raw("}");
  };
  if (m_haveSelf) emit(m_self, m_mac, 0, 0, true);
  for (auto& n : m_nodes) if (n.used) emit(n.p, n.mac, n.rssi, now - n.lastMs, false);
  j.raw("],\"near\":[");
  first = true;
  for (auto& n : m_near) {
    if (!n.used || j.nearlyFull(160)) continue;
    if (!first) j.raw(",");
    first = false;
    char id[8]; snprintf(id, sizeof id, "%02X%02X%02X", n.mac[3], n.mac[4], n.mac[5]);
    j.raw("{"); j.ks("id", id); j.ks("name", n.name); j.ks("lbl", n.label); j.ki("st", n.st); j.ki("rssi", n.rssi); j.ki("ago", (long)((now - n.lastMs) / 1000)); j.raw("}");
  }
  j.raw("]}");
}
