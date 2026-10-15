#include "domain/lite/drive_mix_lite.h"

namespace cq {

namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Spec 02 §A4.1: the driver's steering by stick angle. Stick distance m sets the outer track; the sideways
// share t (‰) slows the inner track: 1000‰ straight, 0 at 75° (t = 789, pivot), -1000‰ fully sideways (spin
// only within ±15° of sideways). Reversing mirrors it so both tracks run backwards. Pure axes give the old mix.
Tracks angleMixLite(int16_t v, int16_t w) {
  const int16_t a = absI(v), b = absI(w);
  const int16_t big = a > b ? a : b, small = a > b ? b : a;
  const int16_t m = clampI(static_cast<int16_t>(big + small * 3 / 8), 0, 100);
  const int16_t t = static_cast<int16_t>(static_cast<int32_t>(b) * 1000 / (a + b));  // b != 0 here
  constexpr int16_t kPivotT = 789;  // tan(75°) / (1 + tan(75°))
  const int16_t r = t <= kPivotT ? static_cast<int16_t>(1000 - static_cast<int32_t>(t) * 1000 / kPivotT)
                                 : static_cast<int16_t>(-static_cast<int32_t>(t - kPivotT) * 1000 / (1000 - kPivotT));
  const int16_t inner = static_cast<int16_t>(static_cast<int32_t>(m) * r / 1000);
  if (v >= 0) return w > 0 ? Tracks{inner, m} : Tracks{m, inner};
  return w > 0 ? Tracks{static_cast<int16_t>(-m), static_cast<int16_t>(-inner)}
               : Tracks{static_cast<int16_t>(-inner), static_cast<int16_t>(-m)};
}
}  // namespace

// w_steer != 0: the driver steers (heading hold then gives w_ctrl = 0) → angle mix. w_steer == 0: straight
// driving + gyro correction → the arcade mix, unchanged (turning priority: speed given up to keep the turn).
// turn_full_pct no longer scales the stick (§A4.1); kept in the signature for the setting's compatibility.
Tracks mixLite(int8_t v8, int8_t w_steer, int8_t w_ctrl, uint8_t /*turn_full_pct*/) {
  int16_t v = v8;
  if (w_steer != 0) return angleMixLite(v, w_steer);
  const int16_t w = clampI(w_ctrl, -100, 100);
  const int16_t excess = static_cast<int16_t>(absI(v) + absI(w) - 100);
  if (excess > 0) v = static_cast<int16_t>(v > 0 ? v - excess : v + excess);
  return Tracks{static_cast<int16_t>(v - w), static_cast<int16_t>(v + w)};
}

Tracks capLite(Tracks t, uint8_t cap_pct, uint16_t dist_mm, uint8_t stop_cm, uint8_t slow_cm, bool brake_on,
               bool& capped) {
  capped = false;
  const int16_t m = absI(t.l) > absI(t.r) ? absI(t.l) : absI(t.r);
  if (m > cap_pct && m > 0) {
    t.l = static_cast<int16_t>(static_cast<int32_t>(t.l) * cap_pct / m);
    t.r = static_cast<int16_t>(static_cast<int32_t>(t.r) * cap_pct / m);
    capped = true;
  }
  const uint16_t stop_mm = static_cast<uint16_t>(stop_cm) * 10u;
  const uint16_t slow_mm = static_cast<uint16_t>(slow_cm) * 10u;
  if (brake_on && t.l + t.r > 0 && dist_mm < 0xFFFE && dist_mm < slow_mm && slow_mm > stop_mm) {
    const int32_t f = dist_mm <= stop_mm ? 0 : static_cast<int32_t>(dist_mm - stop_mm) * 1000 / (slow_mm - stop_mm);
    t.l = static_cast<int16_t>(t.l * f / 1000);
    t.r = static_cast<int16_t>(t.r * f / 1000);
  }
  return t;
}

int16_t RampLite::step(int16_t target_pct, uint16_t accel_pct_s) {
  const int16_t target = static_cast<int16_t>(target_pct * 10);
  const bool same_sign = m_pm == 0 || (target >= 0) == (m_pm > 0);
  const bool accelerating = same_sign && absI(target) > absI(m_pm);
  int16_t max_step = static_cast<int16_t>((accelerating ? accel_pct_s : accel_pct_s * 2u) / 10u);
  if (max_step < 1) max_step = 1;
  const int16_t diff = static_cast<int16_t>(target - m_pm);
  m_pm = static_cast<int16_t>(m_pm + clampI(diff, static_cast<int16_t>(-max_step), max_step));
  return m_pm;
}

int16_t toPwmLite(int16_t pm, bool moving, uint8_t start_pwm, uint8_t keep_pwm) {
  const int16_t mag = absI(pm);
  if (mag < 5) return 0;
  const int16_t base = moving ? keep_pwm : start_pwm;
  int32_t pwm = base + static_cast<int32_t>(mag > 1000 ? 1000 : mag) * (255 - base) / 1000;
  if (pwm > 255) pwm = 255;
  return static_cast<int16_t>(pm < 0 ? -pwm : pwm);
}

}  // namespace cq
