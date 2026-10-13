#pragma once
#include <stdint.h>

#include "domain/util/math_util.h"

namespace cq {

// "Moving" = motor output within the last 300 ms. Before the first motion it is never moving
// (a plain timestamp starting at 0 read as moving for the first 300 ms after boot).
class MotionTimer {
 public:
  static constexpr uint32_t kHoldMs = 300;
  void mark(uint32_t now_ms) {
    m_last_ms = now_ms;
    m_seen = true;
  }
  bool moving(uint32_t now_ms) const { return m_seen && !elapsed(now_ms, m_last_ms, kHoldMs); }

 private:
  uint32_t m_last_ms = 0;
  bool m_seen = false;
};

}  // namespace cq
