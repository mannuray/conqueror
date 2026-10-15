#include "domain/lite/annunciator_lite.h"

namespace cq {

void AnnunciatorLite::update(const LiteUiState& s, uint32_t now, LedSequencer& led) {
  const LiteUiState& p = m_prev;
  if (m_first || s.locked != p.locked || s.tilted_or_lifted != p.tilted_or_lifted || s.batt_crit != p.batt_crit ||
      s.batt_low != p.batt_low) {
    if (s.locked) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kSolid, kAlarm, 0, 0}, now);
    } else if (s.tilted_or_lifted) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kAlarm, 250, 0}, now);
    } else if (s.batt_crit) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kAlarm, 1000, 0}, now);
    } else if (s.batt_low) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kLow, 2000, 0, 150}, now);
    } else {
      led.clear(LedLayer::kAlarm);
    }
  }
  if (m_first || s.blocked != p.blocked) {
    if (s.blocked) {
      led.set(LedLayer::kAttention, {proto::LedMode::kBlink, kAlarm, 200, 0}, now);
    } else {
      led.clear(LedLayer::kAttention);
    }
  }
  if (m_first || s.owner != p.owner || s.synced != p.synced) {
    Rgb c = kReady;
    if (s.owner == proto::Owner::kIr) {
      c = kIr;
    } else if (s.owner == proto::Owner::kNetwork) {
      c = kHuman;
    } else if (!s.synced) {
      c = kNoLink;
    }
    led.set(LedLayer::kBase, {proto::LedMode::kSolid, c, 0, 0}, now);
  }
  const uint8_t speed = s.speed_pct > 100 ? 100 : s.speed_pct;
  led.setBaseStyle(!s.driving, s.batt_crit ? 1000 : 2000,
                   static_cast<uint8_t>(LedSequencer::kMinLevelPct + (100u - LedSequencer::kMinLevelPct) * speed / 100u));
  m_prev = s;
  m_first = false;
}

}  // namespace cq
