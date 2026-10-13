#pragma once
#include <stdint.h>

namespace cq {

// Mode button (D2). Held from boot: 5 s → WiFi reset request, 15 s → admin reset request (on release,
// a tick at each threshold). After boot: a short press (30 ms..2 s) is BUTTON_PRESS; longer is ignored.
class BootButton {
 public:
  enum class Action : uint8_t { kNone, kPress, kWifiThreshold, kAdminThreshold, kWifiReset, kAdminReset };
  static constexpr uint16_t kBootWindowMs = 500;
  static constexpr uint16_t kWifiHoldMs = 5000;
  static constexpr uint16_t kAdminHoldMs = 15000;
  static constexpr uint8_t kDebounceMs = 20;

  Action update(bool raw_pressed, uint32_t now_ms);

 private:
  bool m_stable = false;
  bool m_raw = false;
  uint32_t m_raw_since_ms = 0;
  bool m_first = true;
  bool m_boot_hold = false;
  uint32_t m_press_ms = 0;
  uint8_t m_thresholds = 0;
};

}  // namespace cq
