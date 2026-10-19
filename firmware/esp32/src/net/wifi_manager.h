// Station / own AP + mDNS (bridge LLD R2.6, Plan M R9): up to 3 saved networks in NVS (seeded from secrets.h), join
// the strongest visible one, own AP after 10 s or when forced (double press / confirmed wifi_mode). Arduino loop
// only; the link task talks to it through wifiPost / wifiPoll / wifiSnapshot (wsSend is link-task only).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/wifi_policy.h"
#include "core/wifi_store.h"
#include "core/wifi_types.h"

namespace cq {
namespace net {

using WifiRequest = core::WifiRequest;  // value types live in core (bridge LLD R15, app/ never includes net/)
using WifiResult = core::WifiResult;
using WifiSnapshot = core::WifiSnapshot;

bool wifiPost(const WifiRequest& r);  // link task -> loop; false when the queue is full
bool wifiPoll(WifiResult& r);         // loop -> link task
void wifiSnapshot(WifiSnapshot& out);

class WifiManager {
 public:
  void begin(uint32_t now_ms);
  void step(uint32_t now_ms, bool has_clients);
  core::WifiMode mode() const { return m_policy.mode(); }
  const char* modeName() const;
  void ip(char* out, size_t len) const;  // "192.168.1.42" or "" when not up
  int rssi() const;

 private:
  void startMdns();
  void loadStore();
  bool saveStore();
  void startScan(bool join, int fd, int32_t id);
  void collectScan(int n);
  void joinStrongest();
  void startAp(bool forced);
  void handle(WifiRequest& r, uint32_t now_ms);
  void finishScan();
  void doAction(core::WifiAction a);
  void reportMode();
  bool stored(core::StoreResult res, const WifiRequest& r);
  void handleSave(const WifiRequest& r, uint32_t now_ms);
  void handleForget(const WifiRequest& r, uint32_t now_ms);
  void handleApPassword(const WifiRequest& r, uint32_t now_ms);
  void applyDeferred(uint32_t now_ms);
  void refreshSnapshot();
  bool inUse(const char* ssid) const;

  core::WifiPolicy m_policy;
  core::WifiStore m_store;
  bool m_mdns = false;
  bool m_scanning = false;
  bool m_join_scan = false;
  int m_scan_fd = -1;
  int32_t m_scan_id = -1;
  core::ScanEntry m_scan[12] = {};
  uint8_t m_n_scan = 0;
  // A change that drops the WebSocket is applied 500 ms after its reply, so the reply reaches the page.
  uint8_t m_defer = 0;  // 0 none, 1 force AP, 2 force home, 3 reconnect
  uint32_t m_defer_at = 0;
  uint32_t m_last_snapshot_ms = 0;
  core::WifiMode m_reported_mode = core::WifiMode::kConnecting;  // Plan O: mode changes go to the event log
};

}  // namespace net
}  // namespace cq
