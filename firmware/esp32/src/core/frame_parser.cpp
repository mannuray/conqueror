#include "core/frame_parser.h"

namespace cq {
namespace core {

void FrameParser::reset() {
  m_state = State::kSync;
  m_raw_n = 0;
  m_replay_n = 0;
}

// LEN byte: too long -> resync (a SYNC byte here is a fresh start).
void FrameParser::onLen(uint8_t b) {
  if (b <= proto::kMaxPayload) {
    m_len = b;
    m_state = State::kType;
    return;
  }
  ++m_stats.bad_len;
  if (b == proto::kSync) {
    m_raw_n = 0;  // treat as a fresh SYNC; stay in kLen
  } else {
    m_state = State::kSync;
  }
}

// CRC byte: a frame, or a bad CRC - then the bytes after the first SYNC inside it are replayed.
FrameParser::Step FrameParser::onCrc(uint8_t b) {
  m_state = State::kSync;
  const uint8_t head[3] = {m_len, m_type, m_seq};
  if (b == proto::crc8(m_buf, m_len, proto::crc8(head, 3))) {
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

void FrameParser::onHeader(uint8_t b) {  // TYPE / SEQ bytes
  if (m_state == State::kType) {
    m_type = b;
    m_state = State::kSeq;
    return;
  }
  m_seq = b;
  m_got = 0;
  m_state = m_len ? State::kPayload : State::kCrc;
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
    case State::kLen: onLen(b); break;
    case State::kType:
    case State::kSeq: onHeader(b); break;
    case State::kPayload:
      m_buf[m_got++] = b;
      if (m_got == m_len) m_state = State::kCrc;
      break;
    case State::kCrc:
    default: return onCrc(b);
  }
  return Step::kNone;
}

}  // namespace core
}  // namespace cq
