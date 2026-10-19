#include "net/wifi_manager.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "board_config.h"
#include "net/log.h"
#include "secrets.h"

namespace cq {
namespace net {
namespace {
QueueHandle_t g_req = nullptr;
QueueHandle_t g_res = nullptr;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
WifiSnapshot g_snap;
constexpr uint32_t kDeferMs = 500;

void post(WifiResult::Kind kind, const WifiRequest& r, const char* text = "") {
  if (g_res == nullptr || r.fd < 0) return;
  WifiResult res;
  res.kind = kind;
  res.fd = r.fd;
  res.id = r.id;
  res.req = static_cast<uint8_t>(r.kind);
  strncpy(res.text, text, sizeof(res.text) - 1);
  xQueueSendToBack(g_res, &res, 0);
}

const char* storeError(core::StoreResult s) {
  switch (s) {
    case core::StoreResult::kBadSsid: return "network name must be 1-32 characters";
    case core::StoreResult::kBadPassword: return "password must be 8-63 characters";
    case core::StoreResult::kFull: return "3 networks saved: pick one to replace";
    case core::StoreResult::kBadSlot: return "no such slot";
    case core::StoreResult::kOk:
    default: return "";
  }
}
}  // namespace

bool wifiPost(const WifiRequest& r) { return g_req != nullptr && xQueueSendToBack(g_req, &r, 0) == pdTRUE; }

bool wifiPoll(WifiResult& r) { return g_res != nullptr && xQueueReceive(g_res, &r, 0) == pdTRUE; }

void wifiSnapshot(WifiSnapshot& out) {
  taskENTER_CRITICAL(&g_mux);
  out = g_snap;
  taskEXIT_CRITICAL(&g_mux);
}

void WifiManager::begin(uint32_t now_ms) {
  g_req = xQueueCreate(4, sizeof(WifiRequest));
  g_res = xQueueCreate(4, sizeof(WifiResult));
  loadStore();
  WiFi.persistent(false);  // credentials come from our own NVS store, not the SDK's
  WiFi.setHostname(board::kHostname);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds 100+ ms latency to drive messages
  // design-notes §38: 15 dBm (not 20) lowers the current peaks on tank power; plenty indoors.
  WiFi.setTxPower(WIFI_POWER_15dBm);
  m_policy.begin(now_ms);
  startScan(true, -1, -1);
  refreshSnapshot();
}

void WifiManager::loadStore() {
  Preferences p;
  bool ok = false;
  if (p.begin("cq_wifi", true)) {
    uint8_t blob[core::WifiStore::kBlobSize];
    const size_t n = p.getBytes("store", blob, sizeof(blob));
    ok = m_store.decode(blob, n);
    p.end();
  }
  if (!ok) {  // first boot, or an unreadable store (review focus 2): start from secrets.h
    m_store.seed(CQ_WIFI_SSID, CQ_WIFI_PASSWORD, CQ_AP_PASSWORD);
    saveStore();
    logf("wifi: store seeded from secrets.h");
  }
  logf("wifi: %u saved network(s)", static_cast<unsigned>(m_store.count()));
}

bool WifiManager::saveStore() {
  uint8_t blob[core::WifiStore::kBlobSize];
  const size_t n = m_store.encode(blob);
  Preferences p;
  if (!p.begin("cq_wifi", false)) return false;
  const bool ok = p.putBytes("store", blob, n) == n;
  p.end();
  memset(blob, 0, sizeof(blob));
  return ok;
}

void WifiManager::startScan(bool join, int fd, int32_t id) {
  if (fd >= 0) {
    m_scan_fd = fd;
    m_scan_id = id;
  }
  m_join_scan = m_join_scan || join;
  if (m_scanning) return;
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {  // e.g. while the station is joining (review I3)
    logf("wifi: scan could not start");
    if (m_scan_fd >= 0) {
      WifiRequest r;
      r.kind = WifiRequest::kScan;
      r.fd = m_scan_fd;
      r.id = m_scan_id;
      post(WifiResult::kFailed, r, "scan busy, try again in a few seconds");
      m_scan_fd = -1;
    }
    return;
  }
  m_scanning = true;
}

void WifiManager::collectScan(int n) {
  m_n_scan = 0;
  for (int i = 0; i < n; ++i) {
    core::ScanEntry e{};
    strncpy(e.ssid, WiFi.SSID(i).c_str(), sizeof(e.ssid) - 1);
    if (e.ssid[0] == '\0') continue;  // hidden network
    const int rssi = WiFi.RSSI(i);
    e.rssi = static_cast<int8_t>(rssi < -127 ? -127 : rssi);
    e.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    // keep the strongest 12, strongest first (insertion)
    uint8_t pos = m_n_scan;
    while (pos > 0 && m_scan[pos - 1].rssi < e.rssi) --pos;
    if (pos >= 12) continue;
    const uint8_t last = m_n_scan < 12 ? m_n_scan : 11;
    for (uint8_t j = last; j > pos; --j) m_scan[j] = m_scan[j - 1];
    m_scan[pos] = e;
    if (m_n_scan < 12) ++m_n_scan;
  }
}

void WifiManager::joinStrongest() {
  const int slot = m_store.pickStrongest(m_scan, m_n_scan);
  if (slot < 0) {
    logf("wifi: no saved network visible");
    return;
  }
  WiFi.begin(m_store.ssid(static_cast<uint8_t>(slot)), m_store.pass(static_cast<uint8_t>(slot)));
  logf("wifi: joining \"%s\"", m_store.ssid(static_cast<uint8_t>(slot)));  // the name only, never the password
}

void WifiManager::startAp(bool forced) {
  WiFi.mode(WIFI_AP_STA);  // the station side stays for scans (and, unforced, for the 60 s retries)
  WiFi.softAP(board::kFallbackSsid, m_store.apPass());
  logf("wifi: %s AP \"%s\" at %s", forced ? "own (forced)" : "fallback", board::kFallbackSsid,
       WiFi.softAPIP().toString().c_str());
  startMdns();
}

bool WifiManager::inUse(const char* ssid) const {
  return m_policy.mode() == core::WifiMode::kStation && ssid != nullptr && ssid[0] != '\0' &&
         strcmp(WiFi.SSID().c_str(), ssid) == 0;
}

void WifiManager::handle(WifiRequest& r, uint32_t now_ms) {
  switch (r.kind) {
    case WifiRequest::kStatus:
      refreshSnapshot();
      post(WifiResult::kStatus, r);
      break;
    case WifiRequest::kScan:
      startScan(false, r.fd, r.id);
      break;
    case WifiRequest::kSave: {
      // review I2: the slot's old name counts too (renaming the network in use), and a join in progress restarts
      const bool was_in_use = inUse(r.ssid) || (r.slot >= 0 && inUse(m_store.ssid(static_cast<uint8_t>(r.slot))));
      const core::StoreResult s = m_store.save(r.slot, r.ssid, r.pass);
      if (s != core::StoreResult::kOk) {
        post(WifiResult::kFailed, r, storeError(s));
      } else if (!saveStore()) {
        post(WifiResult::kFailed, r, "could not write flash");
      } else {
        logf("wifi: saved network \"%s\"", r.ssid);
        post(WifiResult::kApplied, r);
        if (was_in_use || m_policy.mode() != core::WifiMode::kStation) {  // new credentials: rejoin (focus 1)
          if (m_policy.mode() != core::WifiMode::kForcedAp) {
            m_defer = 3;
            m_defer_at = now_ms + kDeferMs;
          }
        }
      }
      break;
    }
    case WifiRequest::kForget: {
      bool was_in_use = m_policy.mode() == core::WifiMode::kConnecting;  // review I2: a join may be on those credentials
      if (r.slot < 0) {
        was_in_use = was_in_use || m_policy.mode() == core::WifiMode::kStation;
      } else {
        was_in_use = was_in_use || inUse(m_store.ssid(static_cast<uint8_t>(r.slot)));
      }
      const core::StoreResult s = m_store.forget(r.slot);
      if (s != core::StoreResult::kOk) {
        post(WifiResult::kFailed, r, storeError(s));
      } else if (!saveStore()) {
        post(WifiResult::kFailed, r, "could not write flash");
      } else {
        logf("wifi: forgot %s", r.slot < 0 ? "all networks" : "a network");
        post(WifiResult::kApplied, r);
        if (was_in_use) {
          m_defer = 3;
          m_defer_at = now_ms + kDeferMs;
        }
      }
      break;
    }
    case WifiRequest::kApPassword: {
      const core::StoreResult s = m_store.setApPassword(r.pass);
      if (s != core::StoreResult::kOk) {
        post(WifiResult::kFailed, r, storeError(s));
      } else if (!saveStore()) {
        post(WifiResult::kFailed, r, "could not write flash");
      } else {
        logf("wifi: own AP password changed");
        post(WifiResult::kApplied, r);
        const core::WifiMode m = m_policy.mode();
        if (m == core::WifiMode::kFallbackAp || m == core::WifiMode::kForcedAp) {
          m_defer = m == core::WifiMode::kForcedAp ? 1 : 0;  // restart the AP with the new password
          if (m_defer == 0) WiFi.softAP(board::kFallbackSsid, m_store.apPass());
          m_defer_at = now_ms + kDeferMs;
        }
      }
      break;
    }
    case WifiRequest::kMode:
      post(WifiResult::kApplied, r);
      m_defer = r.ap ? 1 : 2;
      m_defer_at = now_ms + kDeferMs;
      break;
  }
  memset(r.pass, 0, sizeof(r.pass));
}

void WifiManager::applyDeferred(uint32_t now_ms) {
  if (m_defer == 0 || static_cast<int32_t>(now_ms - m_defer_at) < 0) return;
  const uint8_t what = m_defer;
  m_defer = 0;
  if (what == 1) {  // own AP until switched back
    WiFi.disconnect();
    m_policy.forceAp(now_ms);
    startAp(true);
  } else {  // 2 home, 3 reconnect with the new store: as boot
    WiFi.softAPdisconnect(true);
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    m_policy.forceHome(now_ms);
    startScan(true, -1, -1);
    logf("wifi: %s", what == 2 ? "switching to home WiFi" : "reconnecting with the saved networks");
  }
  refreshSnapshot();
}

void WifiManager::step(uint32_t now_ms, bool has_clients) {
  WifiRequest req;
  while (g_req != nullptr && xQueueReceive(g_req, &req, 0) == pdTRUE) handle(req, now_ms);
  applyDeferred(now_ms);
  if (m_scanning) {
    const int n = WiFi.scanComplete();
    if (n >= 0 || n == WIFI_SCAN_FAILED) {
      m_scanning = false;
      collectScan(n < 0 ? 0 : n);
      WiFi.scanDelete();
      if (m_join_scan && m_policy.mode() != core::WifiMode::kForcedAp &&
          m_policy.mode() != core::WifiMode::kStation) {
        joinStrongest();
      }
      m_join_scan = false;
      refreshSnapshot();
      if (m_scan_fd >= 0) {
        WifiRequest r;
        r.kind = WifiRequest::kScan;
        r.fd = m_scan_fd;
        r.id = m_scan_id;
        post(WifiResult::kScan, r);
        m_scan_fd = -1;
      }
    }
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  switch (m_policy.step(now_ms, connected, has_clients)) {
    case core::WifiAction::kStationUp:
    case core::WifiAction::kApOff:
      if (WiFi.getMode() == WIFI_AP_STA) WiFi.mode(WIFI_STA);
      logf("wifi: station up, IP %s, RSSI %d dBm, http://%s.local/", WiFi.localIP().toString().c_str(),
           WiFi.RSSI(), board::kHostname);
      startMdns();
      refreshSnapshot();
      break;
    case core::WifiAction::kStartStation:
      logf("wifi: station lost, rescanning");
      startScan(true, -1, -1);
      break;
    case core::WifiAction::kStartAp:
      startAp(false);
      refreshSnapshot();
      break;
    case core::WifiAction::kRetryStation:
      logf("wifi: retrying the saved networks (AP stays up)");
      startScan(true, -1, -1);
      break;
    case core::WifiAction::kNone:
    default:
      break;
  }
  if (now_ms - m_last_snapshot_ms >= 1000) {
    m_last_snapshot_ms = now_ms;
    refreshSnapshot();
  }
  if (m_policy.mode() != m_reported_mode && m_policy.mode() != core::WifiMode::kConnecting && g_res != nullptr) {
    WifiResult res;  // Plan O: home / own AP / fallback AP into the event log
    res.kind = WifiResult::kModeChanged;
    res.id = static_cast<int32_t>(m_policy.mode());
    if (xQueueSendToBack(g_res, &res, 0) == pdTRUE) m_reported_mode = m_policy.mode();  // full: tried again next step
  }
}

void WifiManager::refreshSnapshot() {
  WifiSnapshot s;
  snprintf(s.mode, sizeof(s.mode), "%s", modeName());
  const bool station = m_policy.mode() == core::WifiMode::kStation;
  snprintf(s.ssid, sizeof(s.ssid), "%s", station ? WiFi.SSID().c_str() : board::kFallbackSsid);
  s.rssi = rssi();
  ip(s.ip, sizeof(s.ip));
  for (uint8_t i = 0; i < core::WifiStore::kSlots; ++i) {
    snprintf(s.saved[i], sizeof(s.saved[i]), "%s", m_store.ssid(i));
  }
  s.n_saved = core::WifiStore::kSlots;  // all 3 slots, "" = free: the page uses the position as the slot
  memcpy(s.scan, m_scan, sizeof(m_scan));
  s.n_scan = m_n_scan;
  taskENTER_CRITICAL(&g_mux);
  g_snap = s;
  taskEXIT_CRITICAL(&g_mux);
}

void WifiManager::startMdns() {
  if (m_mdns) return;
  if (!MDNS.begin(board::kHostname)) {
    logf("mdns: start failed");
    return;
  }
  MDNS.addService("http", "tcp", board::kHttpPort);
  MDNS.enableArduino(board::kOtaPort, true);  // _arduino._tcp for espota
  m_mdns = true;
  logf("mdns: %s.local (_http._tcp 80, _arduino._tcp 3232)", board::kHostname);
}

const char* WifiManager::modeName() const {
  switch (m_policy.mode()) {
    case core::WifiMode::kStation: return "home";
    case core::WifiMode::kFallbackAp: return "ap_fallback";
    case core::WifiMode::kForcedAp: return "ap";
    case core::WifiMode::kConnecting:
    default: return "connecting";
  }
}

void WifiManager::ip(char* out, size_t len) const {
  if (len == 0) return;
  out[0] = '\0';
  IPAddress a;
  const core::WifiMode m = m_policy.mode();
  if (m == core::WifiMode::kStation) a = WiFi.localIP();
  if (m == core::WifiMode::kFallbackAp || m == core::WifiMode::kForcedAp) a = WiFi.softAPIP();
  if (a != IPAddress()) snprintf(out, len, "%s", a.toString().c_str());
}

int WifiManager::rssi() const { return m_policy.mode() == core::WifiMode::kStation ? WiFi.RSSI() : 0; }

}  // namespace net
}  // namespace cq
