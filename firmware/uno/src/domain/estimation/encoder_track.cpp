#include "domain/estimation/encoder_track.h"

namespace cq {

void EncoderTrack::update(uint32_t edges, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us, int8_t dir) {
  if (!m_primed) {
    m_prev_edges = edges;
    m_primed = true;
  }
  const uint32_t delta = edges - m_prev_edges;
  m_prev_edges = edges;
  m_count += dir < 0 ? -static_cast<int32_t>(delta) : static_cast<int32_t>(delta);
  m_recent = edges != 0 && static_cast<uint32_t>(now_us - last_edge_us) < kTimeoutUs;
  m_cps = (m_recent && period_us > 0) ? (dir < 0 ? -1e6f : 1e6f) / static_cast<float>(period_us) : 0.0f;
}

}  // namespace cq
