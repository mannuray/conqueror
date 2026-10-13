// Test helper: build frames with the generated codec (tests never hand-code frame bytes).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cq_protocol.h"

namespace cqtest {

template <class Msg>
size_t frame(const Msg& m, uint8_t seq, uint8_t* out) {
  const uint8_t n = cq::proto::encode(m, out + 4);
  out[0] = cq::proto::kSync;
  out[1] = n;
  out[2] = static_cast<uint8_t>(Msg::kType);
  out[3] = seq;
  out[4 + n] = cq::proto::crc8(out + 1, static_cast<uint8_t>(n + 3));
  return n + 5u;
}

}  // namespace cqtest
