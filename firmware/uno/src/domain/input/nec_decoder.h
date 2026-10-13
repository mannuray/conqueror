#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

// NEC decoder fed with falling-edge timestamps (receiver output is active low, falling = mark start).
// Code is MSB-first as received (matches the ELEGOO/IRremote 2.x constants). Inverse command checked.
class NecDecoder {
 public:
  bool feed(uint32_t t_us, uint32_t now_ms, IrEvent& out);

 private:
  enum class State : uint8_t { kIdle, kData };
  State m_state = State::kIdle;
  uint32_t m_last_us = 0;
  uint32_t m_bits = 0;
  uint8_t m_count = 0;
  uint32_t m_last_code = 0;
  uint32_t m_last_frame_ms = 0;
  bool m_have_code = false;
};

}  // namespace cq
