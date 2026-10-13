#include "domain/safety/safety_supervisor.h"

namespace cq {

namespace {
int16_t clamp255(int16_t v) { return v > 255 ? 255 : (v < -255 ? -255 : v); }
}  // namespace

PwmOutput SafetySupervisor::apply(int16_t pwm_l, int16_t pwm_r, const SafetyInputs& in, SafetyStatus& st) const {
  st = SafetyStatus{false, false, false, false};
  if (in.lifted) {
    st.coast = true;
    return PwmOutput(0, 0, false);  // tracks free-wheel while carried
  }
  if (in.selftest_locked) {  // network, IR and calibration output all pass here: nothing moves
    st.locked = true;
    st.halted = true;
    return PwmOutput(0, 0, true);
  }
  if (in.tilted || in.batt_critical) {
    st.halted = true;
    return PwmOutput(0, 0, true);
  }
  const int16_t l = (in.stall_mask & 0x01) ? 0 : clamp255(pwm_l);
  const int16_t r = (in.stall_mask & 0x02) ? 0 : clamp255(pwm_r);
  const bool range_valid = in.dist_mm < 0xFFFE;
  const bool too_close = range_valid && in.dist_mm < static_cast<uint16_t>(in.stop_cm) * 10u;
  const int16_t forward = static_cast<int16_t>((l + r) / 2);
  if (in.brake_on && too_close && forward > 10) {
    st.blocked = true;
    return PwmOutput(0, 0, true);
  }
  return PwmOutput(l, r, true);
}

}  // namespace cq
