#include "domain/input/nec_decoder.h"

#include "domain/util/math_util.h"

namespace cq {

namespace {
bool within(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }
}  // namespace

bool NecDecoder::feed(uint32_t t_us, uint32_t now_ms, IrEvent& out) {
  const uint32_t dt = t_us - m_last_us;
  m_last_us = t_us;
  if (within(dt, 12500, 14500)) {  // 9 ms mark + 4.5 ms space: frame start
    m_state = State::kData;
    m_bits = 0;
    m_count = 0;
    return false;
  }
  if (within(dt, 10500, 12000)) {  // 9 ms mark + 2.25 ms space: repeat
    m_state = State::kIdle;
    if (!m_have_code || elapsed(now_ms, m_last_frame_ms, 200)) return false;
    m_last_frame_ms = now_ms;
    out = IrEvent{m_last_code, true, now_ms};
    return true;
  }
  if (m_state != State::kData) return false;
  uint32_t bit;
  if (within(dt, 800, 1500)) {
    bit = 0;
  } else if (within(dt, 1800, 2700)) {
    bit = 1;
  } else {
    m_state = State::kIdle;
    return false;
  }
  m_bits = (m_bits << 1) | bit;
  if (++m_count < 32) return false;
  m_state = State::kIdle;
  const uint8_t cmd = static_cast<uint8_t>(m_bits >> 8);
  const uint8_t inv = static_cast<uint8_t>(m_bits);
  if (static_cast<uint8_t>(cmd ^ inv) != 0xFF) return false;
  m_last_code = m_bits;
  m_have_code = true;
  m_last_frame_ms = now_ms;
  out = IrEvent{m_bits, false, now_ms};
  return true;
}

}  // namespace cq
