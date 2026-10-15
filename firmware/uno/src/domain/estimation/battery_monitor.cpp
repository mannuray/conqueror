#include "domain/estimation/battery_monitor.h"

#include "domain/util/math_util.h"

namespace cq {

BatteryMonitor::Level BatteryMonitor::update(uint16_t adc, uint16_t mv_per_count_x100, uint16_t low_mv,
                                             uint16_t crit_mv, uint16_t hyst_mv, uint32_t now_ms) {
  const uint32_t mv = (static_cast<uint32_t>(adc) * mv_per_count_x100 + 50u) / 100u;
  m_samples_mv[m_idx] = mv > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(mv);
  m_idx = static_cast<uint8_t>((m_idx + 1) % kAvgSamples);
  if (m_count < kAvgSamples) ++m_count;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < m_count; ++i) sum += m_samples_mv[i];  // slots 0..m_count-1 are filled
  m_mv = static_cast<uint16_t>(sum / m_count);

  const uint32_t low_exit = static_cast<uint32_t>(low_mv) + hyst_mv;   // hysteresis on recovery only
  const uint32_t crit_exit = static_cast<uint32_t>(crit_mv) + hyst_mv;
  Level target;
  // USB only back-feeds ≈ 4.7 V through the regulator (spec 02 §D6.3). Below 5 V counts as USB power only while no
  // battery has been seen since power-on, so a real pack sagging mid-drive still reaches kCritical (motor cut).
  if (m_mv >= 5000u) m_battery_seen = true;
  if (m_mv < 5000u && !m_battery_seen) {
    target = Level::kUsb;
  } else if (m_level == Level::kCritical) {
    target = m_mv > low_exit ? Level::kOk : (m_mv > crit_exit ? Level::kLow : Level::kCritical);
  } else if (m_level == Level::kLow) {
    target = m_mv < crit_mv ? Level::kCritical : (m_mv > low_exit ? Level::kOk : Level::kLow);
  } else {
    target = m_mv < crit_mv ? Level::kCritical : (m_mv < low_mv ? Level::kLow : Level::kOk);
  }

  m_changed = false;
  if (target == m_level) {
    m_candidate = target;
    return m_level;
  }
  if (target != m_candidate) {
    m_candidate = target;
    m_candidate_since_ms = now_ms;
  }
  const bool immediate = target == Level::kUsb && m_level == Level::kOk;
  if (immediate || elapsed(now_ms, m_candidate_since_ms, 2000)) {
    m_level = target;
    m_changed = true;
  }
  return m_level;
}

}  // namespace cq
