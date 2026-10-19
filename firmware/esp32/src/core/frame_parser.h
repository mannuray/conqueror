// Resyncing body-link frame parser (bridge LLD R2.6). Same frames, order and counters as Plan A's
// conqueror_protocol.framing.FrameParser: after a bad length the parser hunts for SYNC again; after a
// bad CRC the bytes received since the false SYNC are re-scanned, so a good frame hidden behind a
// bad header is not lost. Pure C++ (host-tested on protocol/vectors/).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace core {

struct FrameView {
  uint8_t type;
  uint8_t seq;
  uint8_t len;
  const uint8_t* payload;  // valid only inside the on_frame callback
};

struct ParserStats {
  uint32_t frames = 0;
  uint32_t bad_crc = 0;
  uint32_t bad_len = 0;
};

class FrameParser {
 public:
  // Calls on_frame(const FrameView&) for every good frame, in order.
  template <class OnFrame>
  void feed(const uint8_t* data, size_t len, OnFrame&& on_frame) {
    for (size_t i = 0; i < len; ++i) feedByte(data[i], on_frame);
  }
  const ParserStats& stats() const { return m_stats; }
  void reset();

 private:
  enum class State : uint8_t { kSync, kLen, kType, kSeq, kPayload, kCrc };
  enum class Step : uint8_t { kNone, kFrame, kReplay };
  static constexpr size_t kRawMax = proto::kMaxPayload + 4;  // len type seq payload crc
  static constexpr size_t kWorkMax = kRawMax + 1;

  template <class OnFrame>
  void feedByte(uint8_t b, OnFrame& on_frame) {
    uint8_t work[kWorkMax];
    size_t n = 0;
    size_t pos = 0;
    work[n++] = b;
    while (pos < n) {
      const Step s = step(work[pos++]);
      if (s == Step::kFrame) {
        const FrameView v{m_type, m_seq, m_len, m_buf};
        on_frame(v);
      } else if (s == Step::kReplay) {
        // replay = bytes after the next SYNC in the false frame; always shorter than what was
        // consumed since that SYNC, so the loop terminates and fits kWorkMax.
        uint8_t rest[kWorkMax];
        const size_t rest_n = n - pos;
        for (size_t i = 0; i < rest_n; ++i) rest[i] = work[pos + i];
        n = 0;
        for (size_t i = 0; i < m_replay_n; ++i) work[n++] = m_replay[i];
        for (size_t i = 0; i < rest_n && n < kWorkMax; ++i) work[n++] = rest[i];
        pos = 0;
      }
    }
  }
  Step step(uint8_t b);
  void onLen(uint8_t b);
  void onHeader(uint8_t b);
  Step onCrc(uint8_t b);

  State m_state = State::kSync;
  uint8_t m_len = 0;
  uint8_t m_type = 0;
  uint8_t m_seq = 0;
  uint8_t m_got = 0;
  uint8_t m_buf[proto::kMaxPayload] = {};
  uint8_t m_raw[kRawMax] = {};  // every byte since the (possibly false) SYNC
  size_t m_raw_n = 0;
  uint8_t m_replay[kRawMax] = {};
  size_t m_replay_n = 0;
  ParserStats m_stats;
};

}  // namespace core
}  // namespace cq
