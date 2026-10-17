#include "core/roll_learner.h"

#include "core/motion_config.h"

namespace cq {
namespace core {

void RollLearner::begin(int32_t counts_at_stop, uint32_t now_ms) {
  m_on = true;
  m_zero_seen = false;
  m_stop = counts_at_stop;
  m_last = counts_at_stop;
  m_start_ms = now_ms;
  m_change_ms = now_ms;
}

bool RollLearner::observe(int32_t counts, bool driven, uint16_t cpm, uint32_t now_ms, int32_t& mm) {
  if (!m_on) return false;
  if (!driven) {
    m_zero_seen = true;
  } else if (m_zero_seen) {  // someone drives again: the counts are no longer roll-on
    m_on = false;
    return false;
  }
  if (counts != m_last) {
    m_last = counts;
    m_change_ms = now_ms;
  }
  const bool settled = now_ms - m_change_ms >= motion::kRollSettleMs || now_ms - m_start_ms >= motion::kRollWindowMs;
  if (!settled) return false;
  m_on = false;
  const int32_t r = (m_last - m_stop) * 1000 / cpm;
  if (r < 0 || r > motion::kRollMaxMm) return false;
  mm = r;
  return true;
}

uint16_t rollAverage(uint16_t old_mm, int32_t reading_mm) {
  return static_cast<uint16_t>((3 * static_cast<int32_t>(old_mm) + reading_mm + 2) / 4);
}

}  // namespace core
}  // namespace cq
