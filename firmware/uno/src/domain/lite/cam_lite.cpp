#include "domain/lite/cam_lite.h"

namespace cq {

namespace {
int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
// The first stage: the target averaged over the last 5 ticks (100 ms), so a target that arrives in steps (the
// phone's stick, a laptop at 20 Hz) becomes an even ramp before follow() chases it - one stage let the steps
// through as a 10 Hz pulse in the speed that shook the mount (design-notes §137). 5 x 1800 fits in int16.
__attribute__((noinline)) int16_t average(int16_t* hist, uint8_t i, int16_t target) {
  hist[i] = target;
  int16_t sum = 0;
  for (uint8_t k = 0; k < CamLite::kAvgTicks; ++k) sum = static_cast<int16_t>(sum + hist[k]);
  return static_cast<int16_t>((sum + CamLite::kAvgTicks / 2) / CamLite::kAvgTicks);
}

// One axis, one tick (Uno Lite LLD §23), worked on the size of the gap: a quarter of it (rounded up, so it never
// stalls), at most kStepDd, at most kAccelDd faster than the speed already going that way (a turn back starts
// from 0); never past the target, because the step is at most a quarter of the gap; within kSnapDd it lands.
__attribute__((noinline)) void follow(int16_t& out, int16_t& v, int16_t target) {
  const int16_t gap = static_cast<int16_t>(target - out);
  const bool back = gap < 0;
  const int16_t mag = back ? static_cast<int16_t>(-gap) : gap;
  if (mag <= CamLite::kSnapDd) {
    out = target;
    v = 0;
    return;
  }
  int16_t want = static_cast<int16_t>((mag + CamLite::kFollowDiv - 1) / CamLite::kFollowDiv);
  if (want > CamLite::kStepDd) want = CamLite::kStepDd;
  const int16_t speed = back == (v < 0) ? (v < 0 ? static_cast<int16_t>(-v) : v) : 0;
  if (want > speed + CamLite::kAccelDd) want = static_cast<int16_t>(speed + CamLite::kAccelDd);
  v = back ? static_cast<int16_t>(-want) : want;
  out = static_cast<int16_t>(out + v);
}
}  // namespace

void CamLite::set(int16_t pan_dd, int16_t tilt_dd) {
  m_return_pending = false;  // a new command wins over a pending joystick return (full CameraHead)
  m_active = true;
  m_pan = clampI(pan_dd, kPanMin, kPanMax);
  m_tilt = clampI(tilt_dd, kTiltMin, kTiltMax);
}

void CamLite::nudge(int16_t dpan_dd, int16_t dtilt_dd) {
  const int32_t p = static_cast<int32_t>(m_pan) + dpan_dd;
  const int32_t t = static_cast<int32_t>(m_tilt) + dtilt_dd;
  set(static_cast<int16_t>(p < kPanMin ? kPanMin : (p > kPanMax ? kPanMax : p)),
      static_cast<int16_t>(t < kTiltMin ? kTiltMin : (t > kTiltMax ? kTiltMax : t)));
}

void CamLite::joy(int8_t x, int8_t y, bool active, uint32_t now_ms) {
  if (active) {
    m_joy_active = true;
    m_joy_ms = now_ms;
    set(static_cast<int16_t>(kHome - x * 9), static_cast<int16_t>(kHome - y * 9));  // up = smaller tilt angle (§45)
    return;
  }
  m_joy_active = false;
  if (!m_locked) {
    m_return_pending = true;
    m_joy_ms = now_ms;
  }
}

void CamLite::step() {
  follow(m_out_pan, m_v_pan, average(m_hist_pan, m_hist_i, m_pan));
  follow(m_out_tilt, m_v_tilt, average(m_hist_tilt, m_hist_i, m_tilt));
  m_hist_i = static_cast<uint8_t>(m_hist_i + 1 == kAvgTicks ? 0 : m_hist_i + 1);
}

void CamLite::tick(uint32_t now_ms) {
  if (m_joy_active && static_cast<uint32_t>(now_ms - m_joy_ms) >= kJoyTimeoutMs) {
    joy(0, 0, false, m_joy_ms + kJoyTimeoutMs);
  }
  if (m_return_pending && static_cast<uint32_t>(now_ms - m_joy_ms) >= kReturnMs) {
    m_return_pending = false;
    home();
  }
  // follow() reaches the target only through its snap, which also sets the speed to 0 - so "out == target"
  // already means "still"; the speed needs no check of its own (Plan W review M1).
  const bool moving = m_active || m_out_pan != m_pan || m_out_tilt != m_tilt;
  m_active = false;
  if (moving) {
    m_still_since_ms = now_ms;
    m_idle = false;
  } else if (static_cast<uint32_t>(now_ms - m_still_since_ms) >= kDetachAfterMs) {
    m_idle = true;
  }
}

}  // namespace cq
