#include "core/frame_parser.h"

namespace cq {
namespace core {

void FrameParser::reset() {
  m_state = State::kSync;
  m_raw_n = 0;
  m_replay_n = 0;
}

FrameParser::Step FrameParser::step(uint8_t b) {
  if (m_state == State::kSync) {
    if (b == proto::kSync) {
      m_state = State::kLen;
      m_raw_n = 0;
    }
    return Step::kNone;
  }
  if (m_raw_n < kRawMax) m_raw[m_raw_n++] = b;
  switch (m_state) {
    case State::kLen:
      if (b > proto::kMaxPayload) {
        ++m_stats.bad_len;
        if (b == proto::kSync) {
          m_raw_n = 0;  // treat as a fresh SYNC; stay in kLen
        } else {
          m_state = State::kSync;
        }
        return Step::kNone;
      }
      m_len = b;
      m_state = State::kType;
      return Step::kNone;
    case State::kType:
      m_type = b;
      m_state = State::kSeq;
      return Step::kNone;
    case State::kSeq:
      m_seq = b;
      m_got = 0;
      m_state = m_len ? State::kPayload : State::kCrc;
      return Step::kNone;
    case State::kPayload:
      m_buf[m_got++] = b;
      if (m_got == m_len) m_state = State::kCrc;
      return Step::kNone;
    case State::kCrc:
    default: {
      m_state = State::kSync;
      const uint8_t head[3] = {m_len, m_type, m_seq};
      const uint8_t expected = proto::crc8(m_buf, m_len, proto::crc8(head, 3));
      if (b == expected) {
        ++m_stats.frames;
        return Step::kFrame;
      }
      ++m_stats.bad_crc;
      size_t idx = 0;
      while (idx < m_raw_n && m_raw[idx] != proto::kSync) ++idx;
      if (idx >= m_raw_n) {
        m_raw_n = 0;
        return Step::kNone;
      }
      m_replay_n = 0;
      for (size_t i = idx + 1; i < m_raw_n; ++i) m_replay[m_replay_n++] = m_raw[i];
      m_raw_n = 0;
      m_state = State::kLen;
      return Step::kReplay;
    }
  }
}

}  // namespace core
}  // namespace cq
