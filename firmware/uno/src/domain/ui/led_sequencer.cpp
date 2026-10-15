#include "domain/ui/led_sequencer.h"

#include "domain/util/math_util.h"

namespace cq {

namespace {
Rgb scale(const Rgb& c, uint16_t num, uint16_t den) {
  return Rgb{static_cast<uint8_t>(c.r * num / den), static_cast<uint8_t>(c.g * num / den),
             static_cast<uint8_t>(c.b * num / den)};
}
}  // namespace

void LedSequencer::set(LedLayer layer, const LedPattern& p, uint32_t now_ms) {
  const uint8_t i = static_cast<uint8_t>(layer);
  m_p[i] = p;
  m_start_ms[i] = now_ms;
  m_active[i] = true;
}

void LedSequencer::setBaseStyle(bool breathe, uint16_t period_ms, uint8_t level_pct) {
  m_breathe = breathe;
  m_breathe_ms = period_ms < 2 ? 2 : period_ms;
  m_level_pct = level_pct > 100 ? 100 : level_pct;
}

Rgb LedSequencer::raw(uint32_t now_ms, uint8_t& layer) {
  for (uint8_t i = 0; i < kLayers; ++i) {
    if (!m_active[i]) continue;
    const LedPattern& p = m_p[i];
    if (p.duration_ms != 0 && elapsed(now_ms, m_start_ms[i], p.duration_ms)) {
      m_active[i] = false;
      continue;
    }
    layer = i;
    const uint32_t period = p.period_ms == 0 ? 1000u : p.period_ms;
    const uint32_t phase = (now_ms - m_start_ms[i]) % period;
    switch (p.mode) {
      case proto::LedMode::kOff:
        return Rgb{0, 0, 0};
      case proto::LedMode::kSolid:
        return p.color;
      case proto::LedMode::kBlink:
        if (p.flash_ms != 0) {
          if (phase < p.flash_ms) return p.color;
          continue;  // see-through between flashes: a lower layer shows
        }
        return phase < period / 2 ? p.color : Rgb{0, 0, 0};
      case proto::LedMode::kPulse: {
        const uint32_t half = period / 2;
        const uint32_t level = phase < half ? (phase * 255u) / half : ((period - phase) * 255u) / half;
        return scale(p.color, static_cast<uint16_t>(level), 255);
      }
    }
  }
  layer = kLayers;
  return Rgb{0, 0, 0};
}

Rgb LedSequencer::output(uint32_t now_ms) {
  uint8_t layer = kLayers;
  Rgb c = raw(now_ms, layer);
  const uint8_t base = static_cast<uint8_t>(LedLayer::kBase);
  if (layer == base && m_p[base].mode == proto::LedMode::kSolid) {
    uint8_t pct = m_level_pct;
    if (m_breathe) {  // triangle on the global clock: 20 % → 100 % → 20 %
      const uint32_t half = m_breathe_ms / 2u;
      const uint32_t phase = now_ms % m_breathe_ms;
      const uint32_t tri = phase < half ? phase : m_breathe_ms - phase;  // 0..half
      pct = static_cast<uint8_t>(kMinLevelPct + (100u - kMinLevelPct) * tri / half);
    }
    c = scale(c, pct, 100);
  }
  return m_max == 255 ? c : scale(c, m_max, 255);
}

}  // namespace cq
