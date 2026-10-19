#include "core/confirm.h"

namespace cq {
namespace core {

const char* changeName(ChangeKind k) {
  switch (k) {
    case ChangeKind::kWifiSave: return "wifi_save";
    case ChangeKind::kWifiForget: return "wifi_forget";
    case ChangeKind::kWifiMode: return "wifi_mode";
    case ChangeKind::kApPassword: return "ap_password";
    case ChangeKind::kCalibReset: return "calib";
    case ChangeKind::kCalibSet: return "calib";
    case ChangeKind::kSetting: return "setting";
    case ChangeKind::kNone:
    default: return "none";
  }
}

bool ConfirmGate::take(PendingChange& out) {
  if (!m_waiting) return false;
  out = m_change;
  m_waiting = false;
  return true;
}

bool ConfirmGate::expired(uint32_t now_ms, PendingChange& out) {
  if (!m_waiting || now_ms - m_since < kWindowMs) return false;
  out = m_change;
  m_waiting = false;
  return true;
}

bool DoublePress::onPress(uint32_t now_ms) {
  if (m_armed && now_ms - m_first_ms <= kGapMs) {
    m_armed = false;
    return true;
  }
  m_armed = true;
  m_first_ms = now_ms;
  return false;
}

ButtonAction ButtonRouter::onPress(uint32_t now_ms) {
  if (m_gate.waiting()) {
    m_double.reset();  // a confirming press never counts towards a double press
    return ButtonAction::kConfirm;
  }
  return m_double.onPress(now_ms) ? ButtonAction::kToggleWifi : ButtonAction::kNone;
}

}  // namespace core
}  // namespace cq
