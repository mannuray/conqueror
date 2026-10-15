#include "domain/lite/cam_lite.h"

namespace cq {

namespace {
int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
int16_t toward(int16_t out, int16_t target, int16_t step) {
  return clampI(target, static_cast<int16_t>(out - step), static_cast<int16_t>(out + step));
}
}  // namespace

void CamLite::set(int16_t pan_dd, int16_t tilt_dd) {
  m_return_pending = false;  // a new command wins over a pending joystick return (full CameraHead)
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
    set(static_cast<int16_t>(kHome - x * 9), static_cast<int16_t>(kHome + y * 9));
    return;
  }
  m_joy_active = false;
  if (!m_locked) {
    m_return_pending = true;
    m_joy_ms = now_ms;
  }
}

void CamLite::step() {
  m_out_pan = toward(m_out_pan, m_pan, kStepDd);
  m_out_tilt = toward(m_out_tilt, m_tilt, kStepDd);
}

void CamLite::tick(uint32_t now_ms) {
  if (m_joy_active && static_cast<uint32_t>(now_ms - m_joy_ms) >= kJoyTimeoutMs) {
    joy(0, 0, false, m_joy_ms + kJoyTimeoutMs);
  }
  if (m_return_pending && static_cast<uint32_t>(now_ms - m_joy_ms) >= kReturnMs) {
    m_return_pending = false;
    home();
  }
  const bool moving = m_out_pan != m_pan || m_out_tilt != m_tilt;
  if (moving) {
    m_still_since_ms = now_ms;
    m_idle = false;
  } else if (static_cast<uint32_t>(now_ms - m_still_since_ms) >= kDetachAfterMs) {
    m_idle = true;
  }
}

}  // namespace cq
