#pragma once
#include <stdint.h>

namespace cq {

// Edge detection between successive samples of up to 8 input lines (LLD §11.4: the Mega samples
// the IR line with a 20 kHz timer). Starts with every line high (idle level of IR and encoders).
class EdgeDetector {
 public:
  struct Edges {
    uint8_t falling;
    uint8_t rising;
  };

  Edges update(uint8_t level) {
    const Edges e{static_cast<uint8_t>(m_prev & ~level), static_cast<uint8_t>(~m_prev & level)};
    m_prev = level;
    return e;
  }

 private:
  uint8_t m_prev = 0xFF;
};

}  // namespace cq
