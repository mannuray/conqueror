#pragma once
#include <stdint.h>

namespace cq {

class FrameWriter {
 public:
  // Writes sync, len, type, seq, payload, crc into `out`; returns the total length (len + 5).
  uint8_t build(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t* out);
  void setSeq(uint8_t seq) { m_seq = seq; }
  uint8_t lastSeq() const { return m_last_seq; }

 private:
  uint8_t m_seq = 0;
  uint8_t m_last_seq = 0;
};

}  // namespace cq
