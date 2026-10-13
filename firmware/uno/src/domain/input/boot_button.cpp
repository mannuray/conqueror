#include "domain/input/boot_button.h"

#include "domain/util/math_util.h"

namespace cq {

BootButton::Action BootButton::update(bool raw_pressed, uint32_t now_ms) {
  if (m_first) {
    m_first = false;
    m_raw = raw_pressed;
    m_stable = raw_pressed;
    m_raw_since_ms = now_ms;
    if (raw_pressed && now_ms < kBootWindowMs) {
      m_boot_hold = true;
      m_press_ms = now_ms;
    }
    return Action::kNone;
  }
  if (raw_pressed != m_raw) {
    m_raw = raw_pressed;
    m_raw_since_ms = now_ms;
  }
  if (m_raw != m_stable && elapsed(now_ms, m_raw_since_ms, kDebounceMs)) {
    m_stable = m_raw;
    if (m_stable) {
      m_press_ms = now_ms;
      return Action::kNone;
    }
    const uint32_t held = now_ms - m_press_ms;
    if (m_boot_hold) {  // one-shot per boot
      m_boot_hold = false;
      if (held >= kAdminHoldMs) return Action::kAdminReset;
      if (held >= kWifiHoldMs) return Action::kWifiReset;
      return Action::kNone;
    }
    return (held >= 30 && held < 2000) ? Action::kPress : Action::kNone;
  }
  if (m_boot_hold && m_stable) {
    const uint32_t held = now_ms - m_press_ms;
    if (m_thresholds == 0 && held >= kWifiHoldMs) {
      m_thresholds = 1;
      return Action::kWifiThreshold;
    }
    if (m_thresholds == 1 && held >= kAdminHoldMs) {
      m_thresholds = 2;
      return Action::kAdminThreshold;
    }
  }
  return Action::kNone;
}

}  // namespace cq
