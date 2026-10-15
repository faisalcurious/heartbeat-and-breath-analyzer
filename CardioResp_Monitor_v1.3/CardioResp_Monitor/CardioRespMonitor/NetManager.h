// =====================================================================
//  NetManager.h  -  home Wi-Fi first, own "direct" network as fallback
//
//  * At start-up the saved home network is tried (15 s). On success the
//    dashboard is served at the router-assigned (or fixed) IP address and
//    at http://<name>.local.
//  * Without a saved network, or when it cannot be reached, the monitor
//    opens its own network "CardioResp-XXXXXX" with a captive portal
//    (http://192.168.4.1) - exactly the original behaviour.
//  * If the home network disappears for 45 s during use, the direct
//    network is switched on as well; it switches off again when the home
//    network is back and nobody uses the direct network.
//  * Setup from the web page (scan, password, optional fixed IP and name)
//    or from the Serial Monitor (type "setup").
//  * v1.3: the own network (with its captive portal) can stay on while the
//    monitor is on the home network (AP_ALWAYS_ON, switchable on the page),
//    so anybody can join it directly and the dashboard opens by itself.
//  * v1.3: simple, adaptive local names: the first monitor on a network is
//    http://cardio.local, the next cardio2.local, cardio3.local ... (checked
//    with non-blocking mDNS queries, kept across restarts). The old unique
//    name cardio-xxxxxx.local keeps working, and every monitor announces
//    itself (_cardioresp._tcp) so the page can list the others.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "mdns.h"
#include <Preferences.h>
#include "Protocol.h"

struct NetSettings {
  char ssid[33] = "";
  char pass[65] = "";
  char host[32] = "";          // network name: http://<host>.local
  bool useStatic = false;
  uint32_t ip = 0, gw = 0, mask = 0, dns = 0;
};

enum NetTest : uint8_t { NT_IDLE = 0, NT_TESTING = 1, NT_OK = 2, NT_FAIL = 3 };

class NetManager {
 public:
  void begin(const char* apSsid, const char* apPass, const char* defaultHost);
  void loop();
  void serialPoll();                               // Serial commands: help, wifi, setup, forget
  void setSerialHook(bool (*h)(const char*)) { m_hook = h; }   // extra Serial commands (licence key ...); return true when handled
  void printInfo();                                // how to open the dashboard
  bool apActive() const { return m_apOn; }
  bool homeUp() const { return WiFi.status() == WL_CONNECTED; }
  const char* host() const { return m_name; }      // active local name: http://<host>.local
  const char* uniqueHost() const { return m_defHost; }
  bool autoName() const { return !m_set.host[0]; }
  const char* savedSsid() const { return m_set.ssid; }
  bool apKeep() const { return m_apKeep; }
  void setApKeep(bool on);                         // keep the own network (captive portal) on while on the home network
  void setLabel(const char* lbl);                  // bed / room label announced to the other monitors
  void startPeerScan();                            // look for the other monitors on the home network (non-blocking)
  void writePeers(JBuf& j);
  bool startScan();
  void writeScan(JBuf& j);
  bool startTest(const NetSettings& s, char* err, size_t errLen);
  bool makeStatic(char* err, size_t errLen);
  void forget();
  void writeStatus(JBuf& j);                       // full status  (/api/wifi)
  void writeShort(JBuf& j);                        // compact status (WebSocket "s")
  bool parseIp(const char* s, uint32_t& out) const;

 private:
  void load();
  void save(const NetSettings& s);
  void applyIpConfig(const NetSettings& s);
  void beginSta(const NetSettings& s);
  void startAP();
  void stopAP();
  void startMdns(const char* hostName);
  void pickName();                                 // the name to use before / without the network check
  void nameLoop(uint32_t now, bool up);
  void applyName(const char* n, bool save);
  void peerLoop(uint32_t now);
  void updateAlias();
  const char* reasonText(int r) const;
  void finishSerialSetup();

  NetSettings m_set, m_try;
  Preferences m_prefs;
  DNSServer m_dns;
  char m_apSsid[33] = "", m_apPass[24] = "", m_defHost[32] = "";
  bool m_apOn = false, m_bootFailed = false, m_fallback = false, m_retrying = false, m_mdnsOn = false;
  char m_mdnsHost[32] = "";
  // v1.3: own network kept on, adaptive name, other monitors
  bool m_apKeep = true, m_staAuto = false, m_wasUp = false, m_aliasOn = false;
  char m_name[32] = "", m_label[24] = "";
  uint32_t m_downSinceAp = 0, m_aliasIp = 0, m_nameIp = 0, m_nameCheckAt = 0;
  uint8_t m_lastCh = 0;
  mdns_search_once_t* m_nq = nullptr;              // name check in progress
  int m_nIdx = -1;
  char m_nCand[32] = "";
  mdns_search_once_t* m_pq = nullptr;              // peer browse in progress
  struct Peer { char host[32]; char lbl[24]; char id[8]; uint32_t ip; };
  Peer m_peers[12];
  int m_peerN = 0;
  uint32_t m_peerAt = 0;
  bool m_peerEver = false;
  uint32_t m_apOffAt = 0, m_downSince = 0, m_lastRetry = 0, m_retryStart = 0, m_apIdleSince = 0;
  NetTest m_test = NT_IDLE;
  uint32_t m_testStart = 0;
  char m_testMsg[120] = "";
  char m_testIp[16] = "";
  struct Net { char ssid[33]; int8_t rssi; uint8_t open; };
  Net m_nets[20];
  int m_netN = 0;
  bool m_scanning = false;
  char m_line[100];
  int m_lineLen = 0, m_serialStep = 0;
  NetSettings m_serialTry;
  bool (*m_hook)(const char*) = nullptr;
};
