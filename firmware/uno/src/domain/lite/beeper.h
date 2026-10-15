#pragma once
#include <stdint.h>

namespace cq {

// One beep at a time (spec 02 Part D: no melodies in Lite). tick() returns the tone to play, 0 = silent.
class Beeper {
 public:
  void beep(uint16_t hz, uint16_t ms, uint32_t now_ms) {
    m_hz = hz;
    m_until_ms = now_ms + ms;
  }
  uint16_t tick(uint32_t now_ms) {
    if (m_hz != 0 && static_cast<int32_t>(now_ms - m_until_ms) >= 0) m_hz = 0;
    return m_hz;
  }

 private:
  uint16_t m_hz = 0;
  uint32_t m_until_ms = 0;
};

}  // namespace cq
