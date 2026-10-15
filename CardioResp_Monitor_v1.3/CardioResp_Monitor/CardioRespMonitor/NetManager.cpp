// =====================================================================
//  NetManager.cpp
// =====================================================================
#include "NetManager.h"
#include "config.h"
#include "NetNames.h"

#ifndef NAME_BASE
#define NAME_BASE "cardio"         // v1.3: simple local names cardio, cardio2, cardio3 ... (.local)
#endif
#ifndef AP_ALWAYS_ON
#define AP_ALWAYS_ON 1             // v1.3: keep the own network + captive portal on while on the home network
#endif
#ifndef STA_DOWN_AP_S
#define STA_DOWN_AP_S 8            // v1.3: home network gone this long while the own network is on -> stop reconnect scanning
#endif
#ifndef FW_VERSION
#define FW_VERSION "?"
#endif

static const IPAddress AP_IP(192, 168, 4, 1);
static volatile int s_reason = 0;          // last station disconnect reason

static void ipStr(uint32_t v, char* out, size_t n) {
  IPAddress a(v);
  snprintf(out, n, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

bool NetManager::parseIp(const char* s, uint32_t& out) const {
  IPAddress a;
  if (!s || !*s || !a.fromString(s)) return false;
  out = (uint32_t)a;
  return true;
}

// ---------------------------------------------------------------------
void NetManager::load() {
  m_prefs.begin("cardionet", false);
  m_prefs.getString("ssid", m_set.ssid, sizeof(m_set.ssid));
  m_prefs.getString("pass", m_set.pass, sizeof(m_set.pass));
  m_prefs.getString("host", m_set.host, sizeof(m_set.host));
  m_set.useStatic = m_prefs.getBool("st", false);
  m_set.ip = m_prefs.getUInt("ip", 0);
  m_set.gw = m_prefs.getUInt("gw", 0);
  m_set.mask = m_prefs.getUInt("mask", 0);
  m_set.dns = m_prefs.getUInt("dns", 0);
  m_apKeep = m_prefs.getBool("apkeep", AP_ALWAYS_ON != 0);
}

void NetManager::save(const NetSettings& s) {
  m_prefs.putString("ssid", s.ssid);
  m_prefs.putString("pass", s.pass);
  m_prefs.putString("host", s.host);
  m_prefs.putBool("st", s.useStatic);
  m_prefs.putUInt("ip", s.ip);
  m_prefs.putUInt("gw", s.gw);
  m_prefs.putUInt("mask", s.mask);
  m_prefs.putUInt("dns", s.dns);
}

void NetManager::applyIpConfig(const NetSettings& s) {
  if (s.useStatic && s.ip && s.gw && s.mask)
    WiFi.config(IPAddress(s.ip), IPAddress(s.gw), IPAddress(s.mask), IPAddress(s.dns ? s.dns : s.gw));
  else
    WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));   // DHCP
}

void NetManager::beginSta(const NetSettings& s) {
  applyIpConfig(s);
  s_reason = 0;
  // v1.3: start looking on the router's last channel (a found router means no channel hopping for the own network)
  WiFi.begin(s.ssid, s.pass[0] ? s.pass : nullptr, m_lastCh);
#if WIFI_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
}

// ---------------------------------------------------------------------
//  v1.3: local names (mDNS)
// ---------------------------------------------------------------------
void NetManager::pickName() {
  if (m_set.host[0]) { strlcpy(m_name, m_set.host, sizeof m_name); return; }      // fixed name chosen by the user
  char saved[32] = "";
  m_prefs.getString("ahost", saved, sizeof saved);
  strlcpy(m_name, validName(saved) ? saved : NAME_BASE, sizeof m_name);
}

void NetManager::startMdns(const char* hn) {
  if (m_mdnsOn) {
    if (strcmp(m_mdnsHost, hn) != 0 && mdns_hostname_set(hn) == ESP_OK) strlcpy(m_mdnsHost, hn, sizeof m_mdnsHost);
    return;
  }
  m_mdnsOn = MDNS.begin(hn);
  if (!m_mdnsOn) return;
  strlcpy(m_mdnsHost, hn, sizeof m_mdnsHost);
  const char* d = strrchr(m_defHost, '-');
  const char* id = d ? d + 1 : m_defHost;
  char inst[40];
  snprintf(inst, sizeof inst, "CardioResp %s", id);                   // unique instance name per monitor
  MDNS.setInstanceName(inst);
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("cardioresp", "tcp", 80);                          // lets the monitors find each other
  MDNS.addServiceTxt("cardioresp", "tcp", "id", id);
  MDNS.addServiceTxt("cardioresp", "tcp", "lbl", (const char*)m_label);
  MDNS.addServiceTxt("cardioresp", "tcp", "fw", FW_VERSION);
}

// the unique name cardio-xxxxxx.local keeps answering (old bookmarks) next to the simple name
void NetManager::updateAlias() {
  if (!m_mdnsOn) return;
  const uint32_t ip = homeUp() ? (uint32_t)WiFi.localIP() : 0;
  const bool want = ip && strcmp(m_mdnsHost, m_defHost) != 0;
  if (!want) {
    if (m_aliasOn) { mdns_delegate_hostname_remove(m_defHost); m_aliasOn = false; m_aliasIp = 0; }
    return;
  }
  if (m_aliasOn && m_aliasIp == ip) return;
  mdns_ip_addr_t a;
  memset(&a, 0, sizeof a);
  a.addr.type = ESP_IPADDR_TYPE_V4;
  a.addr.u_addr.ip4.addr = ip;
  a.next = nullptr;
  const esp_err_t e = m_aliasOn ? mdns_delegate_hostname_set_address(m_defHost, &a) : mdns_delegate_hostname_add(m_defHost, &a);
  if (e == ESP_OK) { m_aliasOn = true; m_aliasIp = ip; }
}

void NetManager::applyName(const char* n, bool persist) {
  const bool changed = strcmp(m_mdnsHost, n) != 0;
  strlcpy(m_name, n, sizeof m_name);
  if (persist && autoName()) {
    char saved[32] = "";
    m_prefs.getString("ahost", saved, sizeof saved);
    if (strcmp(saved, m_name) != 0) m_prefs.putString("ahost", m_name);    // only write when it changed (flash wear)
  }
  startMdns(m_name);
  updateAlias();
  if (changed) Serial.printf("[NET] Local name: http://%s.local  (also http://%s.local)\n", m_name, m_defHost);
}

// Adaptive name: on (re)connection, when the address changes and every 10 minutes, ask the network (mDNS, non-blocking)
// whether the name is used by another device; the first free one of the candidates is taken.
void NetManager::nameLoop(uint32_t now, bool up) {
  if (m_nq) {
    mdns_result_t* res = nullptr;
    uint8_t n = 0;
    if (!mdns_query_async_get_results(m_nq, 0, &res, &n)) return;           // still asking
    const uint32_t me = (uint32_t)WiFi.localIP();
    const bool mine = !strcmp(m_nCand, m_mdnsHost);                          // the name this monitor is using now
    uint32_t ans[8];
    int na = 0;
    for (mdns_result_t* r = res; r; r = r->next)
      for (mdns_ip_addr_t* a = r->addr; a && na < 8; a = a->next)
        if (a->addr.type == ESP_IPADDR_TYPE_V4) ans[na++] = a->addr.u_addr.ip4.addr;
    const bool taken = nameTaken(ans, na, me, mine);       // NetNames.h (tested on the PC)
    if (res) mdns_query_results_free(res);
    mdns_query_async_delete(m_nq);
    m_nq = nullptr;
    if (!taken) { applyName(m_nCand, true); m_nIdx = -1; m_nameCheckAt = now; return; }
    Serial.printf("[NET] %s.local is used by another device\n", m_nCand);
    m_nIdx++;
  }
  if (!up || !autoName() || !m_mdnsOn) { m_nIdx = -1; return; }
  if (m_nIdx < 0) {
    const uint32_t ip = (uint32_t)WiFi.localIP();
    if (ip == m_nameIp && now - m_nameCheckAt < 600000u) {
      return;
    }
    char hn[64];                                   // the mDNS stack renames itself after a clash it detected: follow it
    if (mdns_hostname_get(hn) == ESP_OK && hn[0] && strcmp(hn, m_mdnsHost) != 0 && strcmp(hn, m_defHost) != 0 && validName(hn)) {
      strlcpy(m_mdnsHost, hn, sizeof m_mdnsHost);
      strlcpy(m_name, hn, sizeof m_name);
    }
    m_nameIp = ip;
    m_nIdx = 0;
  }
  const bool simple = strcmp(m_name, m_defHost) != 0;
  if (!nameCandidate(m_nIdx, simple ? m_name : nullptr, m_nCand, sizeof m_nCand)) {
    applyName(m_defHost, false);                   // cardio ... cardio9 all used: the unique name
    m_nIdx = -1; m_nameCheckAt = now;
    return;
  }
  m_nq = mdns_query_async_new(m_nCand, NULL, NULL, MDNS_TYPE_A, 1500, 4, NULL);
  if (!m_nq) { m_nIdx = -1; m_nameCheckAt = now - 540000u; }    // could not ask: try again in a minute
}

void NetManager::setLabel(const char* lbl) {
  char l[24];
  strlcpy(l, lbl ? lbl : "", sizeof l);
  if (!strcmp(l, m_label)) return;
  strlcpy(m_label, l, sizeof m_label);
  if (m_mdnsOn) MDNS.addServiceTxt("cardioresp", "tcp", "lbl", (const char*)m_label);
}

void NetManager::startPeerScan() {
  if (m_pq || !m_mdnsOn || !homeUp()) return;
  m_pq = mdns_query_async_new(NULL, "_cardioresp", "_tcp", MDNS_TYPE_PTR, 2500, 12, NULL);
}

void NetManager::peerLoop(uint32_t now) {
  if (!m_pq) return;
  mdns_result_t* res = nullptr;
  uint8_t n = 0;
  if (!mdns_query_async_get_results(m_pq, 0, &res, &n)) return;
  const char* d = strrchr(m_defHost, '-');
  const char* myId = d ? d + 1 : m_defHost;
  m_peerN = 0;
  for (mdns_result_t* r = res; r && m_peerN < 12; r = r->next) {
    Peer p;
    memset(&p, 0, sizeof p);
    if (r->hostname) strlcpy(p.host, r->hostname, sizeof p.host);
    for (size_t i = 0; i < r->txt_count; i++) {
      const char* k = r->txt[i].key;
      if (!k) continue;
      const char* v = r->txt[i].value ? r->txt[i].value : "";
      size_t vl = r->txt_value_len ? r->txt_value_len[i] : strlen(v);
      char* dst = !strcmp(k, "lbl") ? p.lbl : !strcmp(k, "id") ? p.id : nullptr;
      size_t cap = !strcmp(k, "lbl") ? sizeof p.lbl : sizeof p.id;
      if (dst) { if (vl >= cap) vl = cap - 1; memcpy(dst, v, vl); dst[vl] = 0; }
    }
    for (mdns_ip_addr_t* a = r->addr; a; a = a->next)
      if (a->addr.type == ESP_IPADDR_TYPE_V4) { p.ip = a->addr.u_addr.ip4.addr; break; }
    if (!p.host[0] && !p.ip) continue;
    if ((p.id[0] && !strcmp(p.id, myId)) || (p.host[0] && !strcmp(p.host, m_mdnsHost))) continue;    // this monitor
    bool dup = false;
    for (int k = 0; k < m_peerN && !dup; k++)
      if ((p.id[0] && !strcmp(m_peers[k].id, p.id)) || (p.host[0] && !strcmp(m_peers[k].host, p.host))) { dup = true; if (!m_peers[k].ip) m_peers[k].ip = p.ip; }
    if (!dup) m_peers[m_peerN++] = p;
  }
  if (res) mdns_query_results_free(res);
  mdns_query_async_delete(m_pq);
  m_pq = nullptr;
  m_peerAt = now;
  m_peerEver = true;
}

void NetManager::writePeers(JBuf& j) {
  j.raw("{");
  j.ki("running", m_pq ? 1 : 0);
  j.ki("age", m_peerEver ? (long)((millis() - m_peerAt) / 1000) : -1);
  j.ks("me", m_name);
  j.ki("home", homeUp() ? 1 : 0);
  j.key("peers"); j.raw("[");
  for (int i = 0; i < m_peerN; i++) {
    const Peer& p = m_peers[i];
    char ip[16] = "";
    if (p.ip) ipStr(p.ip, ip, sizeof ip);
    if (i) j.raw(",");
    j.raw("{"); j.ks("host", p.host); j.ks("ip", ip); j.ks("lbl", p.lbl); j.ks("id", p.id); j.raw("}");
  }
  j.raw("]}");
}

void NetManager::setApKeep(bool on) {
  m_apKeep = on;
  m_prefs.putBool("apkeep", on);
  if (on && !m_apOn) startAP();
  else if (!on && m_apOn && homeUp() && m_test != NT_TESTING) stopAP();
}

void NetManager::startAP() {
  if (m_apOn) return;
  const bool keepSta = homeUp() || m_test == NT_TESTING || m_retrying;
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(m_apSsid, m_apPass, AP_CHANNEL, 0, AP_MAX_CLIENTS);
#if WIFI_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
  delay(100);
  WiFi.AP.enableDhcpCaptivePortal();          // DHCP option 114 / RFC 8908
  m_dns.setErrorReplyCode(DNSReplyCode::NoError);
  m_dns.start(53, "*", AP_IP);
  m_apOn = true;
  m_apIdleSince = millis();
  Serial.printf("[NET] Direct network \"%s\" on (http://192.168.4.1)\n", m_apSsid);
}

void NetManager::stopAP() {
  if (!m_apOn) return;
  m_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  m_apOn = false;
  m_apOffAt = 0;
  m_fallback = false;
  Serial.println(F("[NET] Direct network off - using the home network only"));
}

// ---------------------------------------------------------------------
void NetManager::begin(const char* apSsid, const char* apPass, const char* defaultHost) {
  strlcpy(m_apSsid, apSsid, sizeof(m_apSsid));
  strlcpy(m_apPass, apPass, sizeof(m_apPass));
  strlcpy(m_defHost, defaultHost, sizeof(m_defHost));
  load();
  if (!m_set.ssid[0] && strlen(HOME_WIFI_SSID) > 0) {          // compile-time default
    strlcpy(m_set.ssid, HOME_WIFI_SSID, sizeof(m_set.ssid));
    strlcpy(m_set.pass, HOME_WIFI_PASSWORD, sizeof(m_set.pass));
  }
  if (!m_set.host[0] && strlen(DEVICE_HOSTNAME) > 0) strlcpy(m_set.host, DEVICE_HOSTNAME, sizeof(m_set.host));   // fixed name from config.h
  if (!strcmp(m_set.host, m_defHost)) m_set.host[0] = 0;        // v1.2 stored the unique default name: now the simple adaptive one
  pickName();

  WiFi.persistent(false);                                        // we keep our own settings
  WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
    if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) s_reason = info.wifi_sta_disconnected.reason;
  });
  WiFi.setHostname(m_name);                                      // routers often also resolve http://<name>/ or <name>.lan

  if (m_set.ssid[0]) {
    Serial.printf("[NET] Connecting to home network \"%s\" ...\n", m_set.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    beginSta(m_set);
    const uint32_t t0 = millis();
    while (!homeUp() && millis() - t0 < (uint32_t)STA_CONNECT_TIMEOUT_S * 1000u) delay(100);
    if (homeUp()) {
      WiFi.setSleep(STA_MODEM_SLEEP != 0);                                      // smooth live streaming
      m_staAuto = true;
      m_lastCh = WiFi.channel();
      Serial.printf("[NET] Connected, IP %s\n", WiFi.localIP().toString().c_str());
    } else {
      m_bootFailed = true;
      Serial.printf("[NET] Could not connect to \"%s\": %s\n", m_set.ssid, reasonText(s_reason));
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false);
      m_lastRetry = millis();
    }
  }
  if (!homeUp() || m_apKeep) startAP();                          // v1.3: own network + captive portal also while on the home network
  // on the home network the simple name is claimed only after checking that no other monitor uses it
  startMdns(homeUp() && autoName() ? m_defHost : m_name);
  m_wasUp = false;
}

void NetManager::loop() {
  if (m_apOn) m_dns.processNextRequest();
  const uint32_t now = millis();
  const bool up = homeUp();
  if (up != m_wasUp) {
    if (up) { m_lastCh = WiFi.channel(); m_nameIp = 0; }          // re-check the name on every (re)connection
    updateAlias();
    m_wasUp = up;
  }
  nameLoop(now, up);
  peerLoop(now);

  // ---- connection test started from the page / Serial
  if (m_test == NT_TESTING) {
    if (up && (uint32_t)WiFi.localIP() != 0) {
      m_set = m_try;
      save(m_set);
      m_test = NT_OK;
      ipStr((uint32_t)WiFi.localIP(), m_testIp, sizeof(m_testIp));
      snprintf(m_testMsg, sizeof(m_testMsg), "Connected to \"%s\"", m_set.ssid);
      m_bootFailed = false;
      m_retrying = false;
      WiFi.setAutoReconnect(true);
      m_staAuto = true;
      m_lastCh = WiFi.channel();
      WiFi.setSleep(STA_MODEM_SLEEP != 0);
      pickName();
      WiFi.setHostname(m_name);
      if (autoName()) startMdns(m_mdnsOn ? m_mdnsHost : m_defHost); else applyName(m_name, false);
      m_nameIp = 0;                                                // check the simple name on this network
      if (m_apOn && !m_apKeep) m_apOffAt = now + AP_LINGER_S * 1000u;    // time to read the new address
      Serial.printf("[NET] Connected to \"%s\" - dashboard http://%s  or  http://%s.local\n", m_set.ssid, m_testIp, m_name);
    } else if (now - m_testStart > 20000) {
      m_test = NT_FAIL;
      snprintf(m_testMsg, sizeof(m_testMsg), "%s", reasonText(s_reason));
      Serial.printf("[NET] Connection to \"%s\" failed: %s\n", m_try.ssid, m_testMsg);
      WiFi.disconnect(false);
      if (m_set.ssid[0] && strcmp(m_set.ssid, m_try.ssid) != 0 && !m_apOn) beginSta(m_set);  // back to the old network
      else if (m_apOn) WiFi.mode(WIFI_AP);
      else startAP();
    }
    return;
  }

  // ---- direct network lingering after a successful setup
  if (m_apOffAt && (int32_t)(now - m_apOffAt) >= 0) {
    m_apOffAt = 0;
    if (up) stopAP();
  }

  // ---- v1.3: own network on and the home network gone: the automatic reconnect hops through all channels and
  //      makes the own network (and its captive portal) unusable - stop it after a few seconds and retry in the
  //      background instead (below)
  if (m_apOn && m_set.ssid[0] && !up && !m_retrying) {
    if (!m_downSinceAp) m_downSinceAp = now;
    else if (m_staAuto && now - m_downSinceAp > (uint32_t)STA_DOWN_AP_S * 1000u) {
      Serial.println(F("[NET] Home network lost - own network stays on, retrying in the background"));
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false);
      m_staAuto = false;
      m_lastRetry = now;
    }
  } else m_downSinceAp = 0;

  // ---- home network lost during use -> switch the direct network on
  if (m_set.ssid[0] && !m_apOn) {
    if (up) m_downSince = 0;
    else {
      if (!m_downSince) m_downSince = now;
      if (now - m_downSince > (uint32_t)STA_FALLBACK_S * 1000u) {
        Serial.printf("[NET] Home network lost for %d s\n", STA_FALLBACK_S);
        WiFi.setAutoReconnect(false);
        m_staAuto = false;
        WiFi.disconnect(false);
        startAP();
        m_fallback = true;
        m_lastRetry = now;
        m_downSince = 0;
      }
    }
  }

  // ---- direct mode with a saved network: retry now and then while nobody
  //      uses the direct network (a retry would disturb connected phones)
  if (m_apOn && m_set.ssid[0] && !up && !m_staAuto) {
    const int cli = WiFi.softAPgetStationNum();
    // v1.3: with phones on the own network only one short attempt every 5 minutes (each attempt briefly disturbs it)
    const uint32_t every = (cli == 0 ? (uint32_t)STA_RETRY_S : 300u) * 1000u;
    if (!m_retrying && now - m_lastRetry > every) {
      m_retrying = true;
      m_retryStart = m_lastRetry = now;
      WiFi.mode(WIFI_AP_STA);
      beginSta(m_set);
    }
    if (m_retrying && now - m_retryStart > 15000) {
      m_retrying = false;
      WiFi.disconnect(false);
      WiFi.mode(WIFI_AP);
    }
  }
  if (m_retrying && up) {
    m_retrying = false;
    m_bootFailed = false;
    m_fallback = !m_apKeep;            // direct network goes off once it is idle (unless it is kept on)
    WiFi.setAutoReconnect(true);
    m_staAuto = true;
    WiFi.setSleep(STA_MODEM_SLEEP != 0);
    Serial.printf("[NET] Home network back, IP %s\n", WiFi.localIP().toString().c_str());
  }
  // ---- fallback direct network off again when unused
  if (m_apOn && m_fallback && up && !m_apKeep) {
    if (WiFi.softAPgetStationNum() > 0) m_apIdleSince = now;
    else if (now - m_apIdleSince > 60000) stopAP();
  }

  // ---- scan results
  if (m_scanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      m_netN = 0;
      for (int i = 0; i < n && m_netN < 20; i++) {
        String s = WiFi.SSID(i);
        if (!s.length()) continue;
        int k = 0;
        for (; k < m_netN; k++) if (s == m_nets[k].ssid) break;
        int8_t r = (int8_t)WiFi.RSSI(i);
        if (k < m_netN) { if (r > m_nets[k].rssi) m_nets[k].rssi = r; continue; }
        strlcpy(m_nets[m_netN].ssid, s.c_str(), sizeof(m_nets[0].ssid));
        m_nets[m_netN].rssi = r;
        m_nets[m_netN].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        m_netN++;
      }
      for (int a = 1; a < m_netN; a++)                             // strongest first
        for (int b = a; b > 0 && m_nets[b].rssi > m_nets[b - 1].rssi; b--) { Net t = m_nets[b]; m_nets[b] = m_nets[b - 1]; m_nets[b - 1] = t; }
      WiFi.scanDelete();
      m_scanning = false;
    } else if (n == WIFI_SCAN_FAILED) {
      m_scanning = false;
    }
  }
}

// ---------------------------------------------------------------------
bool NetManager::startScan() {
  if (m_scanning) return true;
  if (m_test == NT_TESTING) return false;
  if (!(WiFi.getMode() & WIFI_MODE_STA)) WiFi.mode(m_apOn ? WIFI_AP_STA : WIFI_STA);
  int16_t r = WiFi.scanNetworks(true, false);
  m_scanning = (r == WIFI_SCAN_RUNNING) || r >= 0;
  return m_scanning;
}

void NetManager::writeScan(JBuf& j) {
  j.raw("{");
  j.ki("running", m_scanning ? 1 : 0);
  j.key("nets"); j.raw("[");
  for (int i = 0; i < m_netN && !j.nearlyFull(64); i++) {
    if (i) j.raw(",");
    j.raw("[\"");                                    // ssid as a JSON string
    for (const char* p = m_nets[i].ssid; *p; p++) { if (*p == '"' || *p == '\\') j.raw("\\"); if ((unsigned char)*p >= 0x20) { char c[2] = {*p, 0}; j.raw(c); } }
    j.raw("\"");
    j.fmt(",%d,%d]", m_nets[i].rssi, m_nets[i].open);
  }
  j.raw("]}");
}

bool NetManager::startTest(const NetSettings& s, char* err, size_t errLen) {
  size_t sl = strlen(s.ssid), pl = strlen(s.pass), hl = strlen(s.host);
  if (sl < 1 || sl > 32) { snprintf(err, errLen, "Network name must be 1-32 characters"); return false; }
  if (pl != 0 && (pl < 8 || pl > 63)) { snprintf(err, errLen, "Wi-Fi passwords have 8-63 characters"); return false; }
  if (hl > 31) { snprintf(err, errLen, "Device name must be at most 31 characters (empty = automatic)"); return false; }
  for (const char* p = s.host; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) { snprintf(err, errLen, "Device name: use a-z, 0-9 and - only"); return false; }
  if (s.useStatic && (!s.ip || !s.gw || !s.mask)) { snprintf(err, errLen, "Fixed address needs IP, gateway and subnet mask"); return false; }
  if (s.useStatic && ((s.ip & s.mask) != (s.gw & s.mask))) { snprintf(err, errLen, "IP address and gateway are not in the same subnet"); return false; }
  if (m_test == NT_TESTING) { snprintf(err, errLen, "A connection test is already running"); return false; }
  m_try = s;
  m_test = NT_TESTING;
  m_testStart = millis();
  m_testIp[0] = 0;
  snprintf(m_testMsg, sizeof(m_testMsg), "Connecting to \"%s\" ...", s.ssid);
  m_retrying = false;
  if (m_scanning) { WiFi.scanDelete(); m_scanning = false; }
  WiFi.disconnect(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(m_apOn ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(s.host[0] ? s.host : m_name);
  m_lastCh = 0;                          // another network: scan all channels
  beginSta(s);
  Serial.printf("[NET] Testing connection to \"%s\"%s\n", s.ssid, s.useStatic ? " (fixed IP)" : "");
  return true;
}

bool NetManager::makeStatic(char* err, size_t errLen) {
  if (!homeUp()) { snprintf(err, errLen, "Not connected to the home network"); return false; }
  m_set.useStatic = true;
  m_set.ip = (uint32_t)WiFi.localIP();
  m_set.gw = (uint32_t)WiFi.gatewayIP();
  m_set.mask = (uint32_t)WiFi.subnetMask();
  m_set.dns = (uint32_t)WiFi.dnsIP();
  save(m_set);
  snprintf(err, errLen, "Address %s is now fixed", WiFi.localIP().toString().c_str());
  return true;
}

void NetManager::forget() {
  m_prefs.clear();
  char host[32];
  strlcpy(host, m_set.host, sizeof(host));
  m_set = NetSettings();
  strlcpy(m_set.host, host, sizeof(m_set.host));
  m_prefs.putString("host", m_set.host);
  m_prefs.putBool("apkeep", m_apKeep);                         // v1.3: keep these two across "forget"
  if (autoName() && strcmp(m_name, m_defHost) != 0) m_prefs.putString("ahost", m_name);
  m_staAuto = false;
  m_test = NT_IDLE;
  m_retrying = false;
  m_fallback = false;
  m_bootFailed = false;
  m_apOffAt = 0;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true);
  startAP();
  WiFi.mode(WIFI_AP);
  Serial.println(F("[NET] Home network forgotten - direct network only"));
}

const char* NetManager::reasonText(int r) const {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
      return "Network not found. The monitor supports 2.4 GHz Wi-Fi only - check the name and move closer to the router.";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
      return "Wrong password, or the router refused the connection.";
    case 0:
      return "No answer from the network (timeout). Check the name and signal strength.";
    default:
      return "Could not connect. Check the network name and password.";
  }
}

// ---------------------------------------------------------------------
void NetManager::writeShort(JBuf& j) {
  j.key("net"); j.raw("{");
  j.ki("home", homeUp() ? 1 : 0);
  j.ki("ap", m_apOn ? 1 : 0);
  j.ks("ssid", m_set.ssid);
  j.ks("host", m_name);
  char ip[16]; ipStr((uint32_t)WiFi.localIP(), ip, sizeof(ip));
  j.ks("ip", homeUp() ? ip : "");
  j.ki("rssi", homeUp() ? WiFi.RSSI() : 0);
  j.raw("}");
}

void NetManager::writeStatus(JBuf& j) {
  const bool up = homeUp();
  char ip[16], gw[16], mask[16], dns[16];
  ipStr(up ? (uint32_t)WiFi.localIP() : m_set.ip, ip, sizeof(ip));
  ipStr(up ? (uint32_t)WiFi.gatewayIP() : m_set.gw, gw, sizeof(gw));
  ipStr(up ? (uint32_t)WiFi.subnetMask() : m_set.mask, mask, sizeof(mask));
  ipStr(up ? (uint32_t)WiFi.dnsIP() : m_set.dns, dns, sizeof(dns));
  j.raw("{");
  j.ks("mode", up ? (m_apOn ? "both" : "home") : "direct");
  j.ks("saved", m_set.ssid);
  j.ki("home", up ? 1 : 0);
  j.ks("ip", (up || m_set.useStatic) ? ip : "");
  j.ks("gw", (up || m_set.useStatic) ? gw : "");
  j.ks("mask", (up || m_set.useStatic) ? mask : "");
  j.ks("dns", (up || m_set.useStatic) ? dns : "");
  j.ki("rssi", up ? WiFi.RSSI() : 0);
  j.ki("static", m_set.useStatic ? 1 : 0);
  j.ks("host", m_name);
  j.ki("auto", autoName() ? 1 : 0);
  j.ks("uniq", m_defHost);
  j.ki("apKeep", m_apKeep ? 1 : 0);
  j.ks("apPass", m_apPass);                    // for the "join the monitor's network" QR code on the page
  j.ki("ap", m_apOn ? 1 : 0);
  j.ks("apSsid", m_apSsid);
  j.ks("apIp", "192.168.4.1");
  j.ki("apCli", m_apOn ? WiFi.softAPgetStationNum() : 0);
  j.ki("apOff", m_apOffAt ? (long)((int32_t)(m_apOffAt - millis()) / 1000) : 0);
  j.ki("bootFailed", m_bootFailed ? 1 : 0);
  j.ki("scanning", m_scanning ? 1 : 0);
  j.key("test"); j.raw("{");
  j.ki("s", m_test); j.ks("ssid", m_try.ssid); j.ks("ip", m_testIp); j.ks("msg", m_testMsg);
  j.raw("}}");
}

void NetManager::printInfo() {
  Serial.println();
  Serial.println(F("================ CardioResp Monitor ================"));
  if (homeUp()) {
    Serial.printf("  Home network  : %s  (signal %d dBm)\n", m_set.ssid, WiFi.RSSI());
    Serial.printf("  Dashboard     : http://%s.local   or   http://%s\n", m_name, WiFi.localIP().toString().c_str());
    Serial.printf("  Also          : http://%s.local\n", m_defHost);
    Serial.printf("  Address       : %s\n", m_set.useStatic ? "fixed" : "from the router (can be fixed on the page)");
  }
  if (m_apOn) {
    Serial.printf("  Direct network: %s\n", m_apSsid);
    Serial.printf("  Password      : %s\n", m_apPass);
    Serial.println(F("  Dashboard     : http://192.168.4.1 (opens automatically after joining)"));
    if (homeUp()) Serial.println(F("  The own network stays on next to the home network (switch it off on the page: Settings -> Network)"));
    if (m_bootFailed) Serial.printf("  Home network \"%s\" not reachable - retrying every %d s while unused\n", m_set.ssid, STA_RETRY_S);
    if (!m_set.ssid[0]) Serial.println(F("  No home network saved: set it on the page, or type  setup  here"));
  }
  Serial.println(F("  Serial commands: help, wifi, setup, forget"));
  Serial.println(F("===================================================="));
}

// ---------------------------------------------------------------------
//  Serial Monitor setup (line based; set the monitor to "Newline")
// ---------------------------------------------------------------------
void NetManager::serialPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (m_lineLen < (int)sizeof(m_line) - 1) m_line[m_lineLen++] = c;
      continue;
    }
    m_line[m_lineLen] = 0;
    m_lineLen = 0;
    String cmd = String(m_line);
    if (m_serialStep == 1) {
      strlcpy(m_serialTry.ssid, m_line, sizeof(m_serialTry.ssid));
      Serial.printf("Password for \"%s\" (empty for an open network): ", m_serialTry.ssid);
      m_serialStep = 2;
      continue;
    }
    if (m_serialStep == 2) {
      strlcpy(m_serialTry.pass, m_line, sizeof(m_serialTry.pass));
      Serial.println(F("(hidden)"));
      m_serialStep = 0;
      finishSerialSetup();
      continue;
    }
    cmd.trim();
    if (m_hook && cmd.length() && m_hook(cmd.c_str())) continue;
    cmd.toLowerCase();
    if (cmd == "setup") {
      m_serialTry = NetSettings();
      strlcpy(m_serialTry.host, m_set.host, sizeof(m_serialTry.host));
      Serial.print(F("Home Wi-Fi name (SSID): "));
      m_serialStep = 1;
    } else if (cmd == "wifi" || cmd == "ip") {
      printInfo();
    } else if (cmd == "forget") {
      forget();
      printInfo();
    } else if (cmd == "help" || cmd.length()) {
      Serial.println(F("Commands:  setup  = enter home Wi-Fi name and password"));
      Serial.println(F("           wifi   = show network status and dashboard address"));
      Serial.println(F("           forget = erase the home Wi-Fi, use the direct network"));
      Serial.println(F("           lickey <32 hex digits> = store the licence AES key;  lic = licence status;  licremove = remove the activation key"));
      Serial.println(F("           clockreset = forget the stored latest time (use only if the phone clock was wrong)"));
    }
  }
}

void NetManager::finishSerialSetup() {
  char err[96];
  if (!startTest(m_serialTry, err, sizeof(err))) Serial.printf("[NET] %s\n", err);
  else Serial.println(F("[NET] Testing (up to 20 s)..."));
}
