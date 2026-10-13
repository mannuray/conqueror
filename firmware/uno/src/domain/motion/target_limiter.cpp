#include "domain/motion/target_limiter.h"

#include "domain/util/math_util.h"

namespace cq {

Limited limitTargets(float l_pct, float r_pct, const Limits& lim) {
  Limited o{l_pct, r_pct, false, false};
  const float m = absf(l_pct) > absf(r_pct) ? absf(l_pct) : absf(r_pct);
  const float cap = lim.speed_cap_pct < lim.degraded_cap_pct ? lim.speed_cap_pct : lim.degraded_cap_pct;
  if (m > cap && m > 0.0f) {
    o.l = l_pct * cap / m;
    o.r = r_pct * cap / m;
    o.capped = true;
  }
  const bool forward = (o.l + o.r) > 0.0f;
  const bool valid = lim.dist_mm < 0xFFFE;
  const float stop_mm = lim.stop_cm * 10.0f;
  const float slow_mm = lim.slow_cm * 10.0f;
  if (lim.brake_on && forward && valid && lim.dist_mm < slow_mm && slow_mm > stop_mm) {
    const float f = clampf((lim.dist_mm - stop_mm) / (slow_mm - stop_mm), 0.0f, 1.0f);
    o.l *= f;
    o.r *= f;
    o.slowed = true;
  }
  return o;
}

}  // namespace cq
