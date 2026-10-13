#include "domain/comms/frame_parser.h"

#include <string.h>

namespace cq {

bool FrameParser::feed(uint8_t b) {
  // Appending behind a replay could need a 53rd slot, so feed() is only legal when nothing is
  // pending; a refused byte leaves the replay untouched (the caller drains first).
  if (pending()) return false;
  // At rest m_n <= 51 (a stored 52nd byte is always a CRC byte, which resets m_n).
  m_raw[m_n] = b;  // the slot it will be stored in anyway
  m_rd = m_n;
  m_end = static_cast<uint8_t>(m_n + 1);
  return true;
}

bool FrameParser::next(uint8_t& budget) {
  // Each step consumes one pending byte; each resync shrinks (stored + pending, <= kRawMax) by
  // at least one, so a received byte costs at most ~700 steps and ~26 CRC passes in total.
  while (budget != 0 && m_rd < m_end) {
    --budget;
#ifdef CQ_HOST_TEST
    ++m_steps;
#endif
    if (step(m_raw[m_rd++])) return true;
  }
  return false;
}

bool FrameParser::step(uint8_t b) {
  if (m_state == State::kSync) {
    if (b == proto::kSync) {
      m_state = State::kLen;
      m_n = 0;
    }
    return false;
  }
  m_raw[m_n++] = b;  // m_n <= m_rd, so this never overwrites a byte still waiting to be stepped
  switch (m_state) {
    case State::kLen:
      if (b > proto::kMaxPayload) {
        ++m_errors.bad_len;
        if (b != proto::kSync) m_state = State::kSync;  // 0xAA: a fresh sync, stay in kLen
        m_n = 0;
        return false;
      }
      m_state = State::kType;
      return false;
    case State::kType:
      m_state = State::kSeq;
      return false;
    case State::kSeq:
      m_state = m_raw[0] ? State::kPayload : State::kCrc;
      return false;
    case State::kPayload:
      if (m_n == 3 + m_raw[0]) m_state = State::kCrc;
      return false;
    case State::kCrc:
      m_state = State::kSync;
      if (proto::crc8(m_raw, static_cast<uint8_t>(m_n - 1)) == b) {
        m_n = 0;  // the frame stays readable in m_raw until the next feed()/next()
        return true;
      }
      ++m_errors.bad_crc;
      resync();
      return false;
    case State::kSync:
      break;
  }
  return false;
}

void FrameParser::resync() {
  uint8_t k = 0;
  while (k < m_n && m_raw[k] != proto::kSync) ++k;
  if (k < m_n) {
    // Step m_raw[k+1 .. m_n) next, then the still-pending tail; move the tail down to follow.
    const uint8_t tail = static_cast<uint8_t>(m_end - m_rd);
    memmove(m_raw + m_n, m_raw + m_rd, tail);
    m_end = static_cast<uint8_t>(m_n + tail);
    m_rd = static_cast<uint8_t>(k + 1);
    m_state = State::kLen;
  }
  m_n = 0;
}

}  // namespace cq
