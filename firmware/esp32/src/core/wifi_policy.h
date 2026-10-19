// WiFi station / fallback AP decisions (bridge LLD Revision 1 "WiFi behaviour", unchanged in R2):
// try the home network; after 10 s (owner 2026-10-16, was 30 s) start the WPA2 AP "conqueror-bridge"; while in
// AP mode retry the home network every 60 s, but only when no WS/TCP/stream client is connected. Plan M: a forced AP
// (double press / confirmed wifi_mode) stays until switched back. Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

constexpr uint32_t kStationTimeoutMs = 10000;  // a normal join takes ~2 s
constexpr uint32_t kHomeRetryMs = 60000;

enum class WifiMode : uint8_t { kConnecting, kStation, kFallbackAp, kForcedAp };
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
      case WifiMode::kConnecting: return stepConnecting(now_ms, sta_connected);
      case WifiMode::kStation: return stepStation(now_ms, sta_connected);
      case WifiMode::kForcedAp: return WifiAction::kNone;  // Plan M: stays on its AP until switched back
      case WifiMode::kFallbackAp:
      default: return stepFallback(now_ms, sta_connected, has_clients);
    }
  }
  WifiAction forceAp(uint32_t now_ms) {  // Plan M: double press / confirmed wifi_mode ap - no station retries
    m_mode = WifiMode::kForcedAp;
    m_since_ms = now_ms;
    return WifiAction::kStartAp;
  }
  WifiAction forceHome(uint32_t now_ms) { return begin(now_ms); }  // as boot: scan, strongest saved, 10 s
  WifiMode mode() const { return m_mode; }

 private:
  WifiAction stepConnecting(uint32_t now_ms, bool sta_connected) {
    if (sta_connected) {
      m_mode = WifiMode::kStation;
      return WifiAction::kStationUp;
    }
    if (now_ms - m_since_ms < kStationTimeoutMs) return WifiAction::kNone;
    m_mode = WifiMode::kFallbackAp;
    m_since_ms = now_ms;
    return WifiAction::kStartAp;
  }
  WifiAction stepStation(uint32_t now_ms, bool sta_connected) {
    if (sta_connected) return WifiAction::kNone;
    m_mode = WifiMode::kConnecting;
    m_since_ms = now_ms;
    return WifiAction::kStartStation;
  }
  WifiAction stepFallback(uint32_t now_ms, bool sta_connected, bool has_clients) {
    if (sta_connected && !has_clients) {  // review M9: never drop the AP under connected phones
      m_mode = WifiMode::kStation;
      return WifiAction::kApOff;  // home network is back: AP off, station up
    }
    if (has_clients || now_ms - m_since_ms < kHomeRetryMs) return WifiAction::kNone;
    m_since_ms = now_ms;
    return WifiAction::kRetryStation;  // AP stays up while the station retries
  }
  WifiMode m_mode = WifiMode::kConnecting;
  uint32_t m_since_ms = 0;
};

}  // namespace core
}  // namespace cq
