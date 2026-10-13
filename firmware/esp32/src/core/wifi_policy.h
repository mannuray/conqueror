// WiFi station / fallback AP decisions (bridge LLD Revision 1 "WiFi behaviour", unchanged in R2):
// try the home network; after 30 s start the WPA2 AP "conqueror-bridge"; while in AP mode retry the
// home network every 60 s, but only when no WS/TCP/stream client is connected. Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

constexpr uint32_t kStationTimeoutMs = 30000;
constexpr uint32_t kHomeRetryMs = 60000;

enum class WifiMode : uint8_t { kConnecting, kStation, kFallbackAp };
enum class WifiAction : uint8_t { kNone, kStartStation, kStationUp, kStartAp, kRetryStation, kApOff };

class WifiPolicy {
 public:
  WifiAction begin(uint32_t now_ms) {
    m_mode = WifiMode::kConnecting;
    m_since_ms = now_ms;
    return WifiAction::kStartStation;
  }
  WifiAction step(uint32_t now_ms, bool sta_connected, bool has_clients) {
    switch (m_mode) {
      case WifiMode::kConnecting:
        if (sta_connected) {
          m_mode = WifiMode::kStation;
          return WifiAction::kStationUp;
        }
        if (now_ms - m_since_ms >= kStationTimeoutMs) {
          m_mode = WifiMode::kFallbackAp;
          m_since_ms = now_ms;
          return WifiAction::kStartAp;
        }
        return WifiAction::kNone;
      case WifiMode::kStation:
        if (!sta_connected) {
          m_mode = WifiMode::kConnecting;
          m_since_ms = now_ms;
          return WifiAction::kStartStation;
        }
        return WifiAction::kNone;
      case WifiMode::kFallbackAp:
      default:
        if (sta_connected && !has_clients) {  // review M9: never drop the AP under connected phones
          m_mode = WifiMode::kStation;
          return WifiAction::kApOff;  // home network is back: AP off, station up
        }
        if (!has_clients && now_ms - m_since_ms >= kHomeRetryMs) {
          m_since_ms = now_ms;
          return WifiAction::kRetryStation;  // AP stays up while the station retries
        }
        return WifiAction::kNone;
    }
  }
  WifiMode mode() const { return m_mode; }

 private:
  WifiMode m_mode = WifiMode::kConnecting;
  uint32_t m_since_ms = 0;
};

}  // namespace core
}  // namespace cq
