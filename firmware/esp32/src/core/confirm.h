// Physical confirmation and the WiFi double press (Plan M, spec 02 §I1 / §I2, bridge LLD R9.3). Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

enum class ChangeKind : uint8_t { kNone, kWifiSave, kWifiForget, kWifiMode, kApPassword, kCalibReset, kSetting, kCalibSet };

struct PendingChange {
  ChangeKind kind = ChangeKind::kNone;
  int fd = -1;          // the client that asked
  int32_t id = -1;      // its command id (ack / confirmed)
  int8_t slot = -1;     // wifi_save / wifi_forget (-1 = first free / all)
  bool ap = false;      // wifi_mode: true = own AP
  char ssid[33] = "";
  char pass[64] = "";   // wifi_save / ap_password - never echoed
  uint8_t setting_id = 0;
  int32_t value = 0;
  uint8_t calib_what = 0;  // 0 counts_per_m, 1 roll
  uint16_t cpm = 0;        // kCalibSet (Plan O): the counts per metre to save
};

const char* changeName(ChangeKind k);

class ConfirmGate {
 public:
  static constexpr uint32_t kWindowMs = 30000;
  void begin(const PendingChange& c, uint32_t now_ms) {
    m_change = c;
    m_since = now_ms;
    m_waiting = true;
  }
  bool waiting() const { return m_waiting; }
  const PendingChange& pending() const { return m_change; }
  bool take(PendingChange& out);
  bool expired(uint32_t now_ms, PendingChange& out);
  void dropClient(int fd) {
    if (m_waiting && m_change.fd == fd) m_waiting = false;
  }

 private:
  PendingChange m_change;
  uint32_t m_since = 0;
  bool m_waiting = false;
};

class DoublePress {
 public:
  static constexpr uint32_t kGapMs = 1000;
  bool onPress(uint32_t now_ms);  // true on the second press within kGapMs (then starts afresh)
  void reset() { m_armed = false; }

 private:
  bool m_armed = false;
  uint32_t m_first_ms = 0;
};

enum class ButtonAction : uint8_t { kNone, kConfirm, kToggleWifi };

class ButtonRouter {
 public:
  explicit ButtonRouter(ConfirmGate& gate) : m_gate(gate) {}
  ButtonAction onPress(uint32_t now_ms);

 private:
  ConfirmGate& m_gate;
  DoublePress m_double;
};

}  // namespace core
}  // namespace cq
