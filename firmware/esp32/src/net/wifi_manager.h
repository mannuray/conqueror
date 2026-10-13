// Station / fallback AP + mDNS (bridge LLD R2.6 WifiManager, MdnsAdvertiser). Arduino loop only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/wifi_policy.h"

namespace cq {
namespace net {

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
  core::WifiPolicy m_policy;
  bool m_mdns = false;
};

}  // namespace net
}  // namespace cq
