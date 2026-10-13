#include "domain/comms/frame_writer.h"

#include "generated/cq_protocol.h"

namespace cq {

uint8_t FrameWriter::build(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t* out) {
  m_last_seq = m_seq;
  out[0] = proto::kSync;
  out[1] = len;
  out[2] = type;
  out[3] = m_seq++;
  for (uint8_t i = 0; i < len; ++i) out[4 + i] = payload[i];
  out[4 + len] = proto::crc8(out + 1, static_cast<uint8_t>(len + 3));
  return static_cast<uint8_t>(len + 5);
}

}  // namespace cq
