#include "domain/motion/mixer.h"

#include "domain/util/math_util.h"

namespace cq {

void mix(float v_pct, float w_pct, const MixerConfig& cfg, float& l_pct, float& r_pct) {
  float v = clampf(v_pct, -100.0f, 100.0f);
  const float authority = 1.0f - (1.0f - cfg.turn_full_speed_pct / 100.0f) * (absf(v) / 100.0f);
  float w = clampf(w_pct * authority, -100.0f, 100.0f);
  const float excess = absf(v) + absf(w) - 100.0f;  // turning priority: give up speed, keep the turn
  if (excess > 0.0f) v = v > 0.0f ? v - excess : v + excess;
  l_pct = v - w;
  r_pct = v + w;
  if (!cfg.pivot_fallback) return;
  float* inner = absf(l_pct) < absf(r_pct) ? &l_pct : (absf(r_pct) < absf(l_pct) ? &r_pct : nullptr);
  if (inner != nullptr && *inner != 0.0f && absf(*inner) < cfg.keep_pct) *inner = 0.0f;
}

}  // namespace cq
