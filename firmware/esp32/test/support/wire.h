// Test helper: drain a FrameTx like the UART would and record the frames that went out.
#pragma once

#include <string.h>

#include "core/frame_parser.h"
#include "core/frame_tx.h"

namespace cqtest {

struct Sent {
  uint8_t type;
  uint8_t seq;
  uint8_t len;
  uint8_t payload[48];
};

struct Wire {
  cq::core::FrameParser parser;
  Sent sent[128];
  size_t n = 0;

  void drain(cq::core::FrameTx& tx) {
    const uint8_t* p = nullptr;
    size_t len = 0;
    while ((len = tx.peek(p)) != 0) {
      parser.feed(p, len, [this](const cq::core::FrameView& f) {
        if (n < 128) {
          sent[n].type = f.type;
          sent[n].seq = f.seq;
          sent[n].len = f.len;
          memcpy(sent[n].payload, f.payload, f.len);
          ++n;
        }
      });
      tx.pop();
    }
  }
  size_t count(cq::proto::FrameType t) const {
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) c += sent[i].type == static_cast<uint8_t>(t) ? 1 : 0;
    return c;
  }
  const Sent* last(cq::proto::FrameType t) const {
    for (size_t i = n; i > 0; --i) {
      if (sent[i - 1].type == static_cast<uint8_t>(t)) return &sent[i - 1];
    }
    return nullptr;
  }
  void clear() { n = 0; }
};

}  // namespace cqtest
