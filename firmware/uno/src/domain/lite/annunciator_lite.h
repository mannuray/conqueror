#pragma once
#include <stdint.h>

#include "domain/ui/led_sequencer.h"
#include "generated/cq_protocol.h"

namespace cq {

struct LiteUiState {
  bool synced;
  proto::Owner owner;
  bool driving;
  uint8_t speed_pct;
  bool locked;            // self-test lock: solid red
  bool tilted_or_lifted;  // red blink 250 ms
  bool batt_crit;         // red blink 1000 ms
  bool batt_low;          // amber 150 ms flash every 2 s over the base (spec 02 LED table)
  bool blocked;           // obstacle stop: fast red blink 100/100 (attention layer)
};

// Spec 02 LED table, Lite subset. Base: no ESP32 link = blue, ready = dim white, network = green, IR = purple;
// breathes when idle, brighter with speed when driving.
class AnnunciatorLite {
 public:
  static constexpr Rgb kNoLink{0, 0, 120};
  static constexpr Rgb kReady{20, 20, 20};
  static constexpr Rgb kHuman{0, 120, 0};
  static constexpr Rgb kIr{80, 0, 120};
  static constexpr Rgb kAlarm{255, 0, 0};
  static constexpr Rgb kLow{200, 110, 0};
  void update(const LiteUiState& s, uint32_t now_ms, LedSequencer& led);

 private:
  LiteUiState m_prev{};
  bool m_first = true;
};

}  // namespace cq
