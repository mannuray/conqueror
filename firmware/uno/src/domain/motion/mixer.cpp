#include "domain/motion/mixer.h"

#include "domain/util/math_util.h"

namespace cq {

namespace {
// Spec 02 §A4.1: the driver's steering by stick angle (same rule as the Uno Lite's angleMixLite). Stick
// distance m = outer track; the sideways share t slows the inner track: 1 straight, 0 at 75° (pivot), -1 fully
// sideways (spin only within ±15° of sideways). Reversing mirrors it. Pure axes give the old arcade mix.
void angleMix(float v, float w, float& l_pct, float& r_pct) {
  const float a = absf(v), b = absf(w);
  const float m = clampf((a > b ? a : b) + 0.375f * (a > b ? b : a), 0.0f, 100.0f);
  const float t = b / (a + b);  // b != 0 here
  constexpr float kPivotT = 0.789f;  // tan(75°) / (1 + tan(75°))
  const float inner = m * (t <= kPivotT ? 1.0f - t / kPivotT : -(t - kPivotT) / (1.0f - kPivotT));
  if (v >= 0.0f) {
    l_pct = w > 0.0f ? inner : m;
    r_pct = w > 0.0f ? m : inner;
  } else {
    l_pct = w > 0.0f ? -m : -inner;
    r_pct = w > 0.0f ? -inner : -m;
  }
}
}  // namespace

void mix(float v_pct, float w_steer_pct, float w_ctrl_pct, const MixerConfig& cfg, float& l_pct, float& r_pct) {
  float v = clampf(v_pct, -100.0f, 100.0f);
  const float w_steer = clampf(w_steer_pct, -100.0f, 100.0f);
  if (w_steer != 0.0f) {  // the driver steers (controllers give w_ctrl 0 then): angle mix, §A4.1
    angleMix(v, clampf(w_steer + w_ctrl_pct, -100.0f, 100.0f), l_pct, r_pct);
  } else {  // straight + gyro correction, primitives, rate loops: arcade mix, unchanged
    const float w = clampf(w_ctrl_pct, -100.0f, 100.0f);
    const float excess = absf(v) + absf(w) - 100.0f;  // turning priority: give up speed, keep the turn
    if (excess > 0.0f) v = v > 0.0f ? v - excess : v + excess;
    l_pct = v - w;
    r_pct = v + w;
  }
  if (!cfg.pivot_fallback) return;
  float* inner = absf(l_pct) < absf(r_pct) ? &l_pct : (absf(r_pct) < absf(l_pct) ? &r_pct : nullptr);
  if (inner != nullptr && *inner != 0.0f && absf(*inner) < cfg.keep_pct) *inner = 0.0f;
}

}  // namespace cq
