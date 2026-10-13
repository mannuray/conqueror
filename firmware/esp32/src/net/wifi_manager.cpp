#include "net/wifi_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "board_config.h"
#include "net/log.h"
#include "secrets.h"

namespace cq {
namespace net {

void WifiManager::begin(uint32_t now_ms) {
  WiFi.persistent(false);  // credentials come from secrets.h, not NVS
  WiFi.setHostname(board::kHostname);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds 100+ ms latency to drive messages
  // design-notes §38: 15 dBm (not 20) lowers the current peaks on tank power; plenty indoors.
  WiFi.setTxPower(WIFI_POWER_15dBm);
  m_policy.begin(now_ms);
  WiFi.begin(CQ_WIFI_SSID, CQ_WIFI_PASSWORD);
  logf("wifi: connecting to \"%s\"", CQ_WIFI_SSID);
}

void WifiManager::step(uint32_t now_ms, bool has_clients) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  switch (m_policy.step(now_ms, connected, has_clients)) {
    case core::WifiAction::kStationUp:
    case core::WifiAction::kApOff:
      if (WiFi.getMode() == WIFI_AP_STA) WiFi.mode(WIFI_STA);
      logf("wifi: station up, IP %s, RSSI %d dBm, http://%s.local/", WiFi.localIP().toString().c_str(),
           WiFi.RSSI(), board::kHostname);
      startMdns();
      break;
    case core::WifiAction::kStartStation:
      logf("wifi: station lost, reconnecting");
      WiFi.reconnect();
      break;
    case core::WifiAction::kStartAp:
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(board::kFallbackSsid, CQ_AP_PASSWORD);
      logf("wifi: home network not reachable - fallback AP \"%s\" at %s", board::kFallbackSsid,
           WiFi.softAPIP().toString().c_str());
      startMdns();
      break;
    case core::WifiAction::kRetryStation:
      logf("wifi: retrying home network (AP stays up)");
      WiFi.begin(CQ_WIFI_SSID, CQ_WIFI_PASSWORD);
      break;
    case core::WifiAction::kNone:
    default:
      break;
  }
}

void WifiManager::startMdns() {
  if (m_mdns) return;
  if (!MDNS.begin(board::kHostname)) {
    logf("mdns: start failed");
    return;
  }
  MDNS.addService("http", "tcp", board::kHttpPort);
  MDNS.addService("cqbridge", "tcp", board::kTcpPort);  // Plan D: discover looks for _cqbridge._tcp
  MDNS.enableArduino(board::kOtaPort, true);             // _arduino._tcp for espota
  m_mdns = true;
  logf("mdns: %s.local (_http._tcp 80, _cqbridge._tcp 3333, _arduino._tcp 3232)",
       board::kHostname);
}

const char* WifiManager::modeName() const {
  switch (m_policy.mode()) {
    case core::WifiMode::kStation:
      return "station";
    case core::WifiMode::kFallbackAp:
      return "fallback_ap";
    case core::WifiMode::kConnecting:
    default:
      return "connecting";
  }
}

void WifiManager::ip(char* out, size_t len) const {
  IPAddress a;
  if (m_policy.mode() == core::WifiMode::kStation) a = WiFi.localIP();
  if (m_policy.mode() == core::WifiMode::kFallbackAp) a = WiFi.softAPIP();
  if (len == 0) return;
  out[0] = '\0';
  if (a != IPAddress()) snprintf(out, len, "%s", a.toString().c_str());
}

int WifiManager::rssi() const {
  return m_policy.mode() == core::WifiMode::kStation ? WiFi.RSSI() : 0;
}

}  // namespace net
}  // namespace cq
