#include "domain/camera/camera_head.h"

#include "domain/util/math_util.h"

namespace cq {

void CameraHead::configure(const CameraConfig& cfg) {
  m_cfg = cfg;
  speed(cfg.speed_dps);
  if (!m_configured) {  // first call: start at home; later calls (setting changes) keep the position
    m_pan.reset(clampf(kHomePan, cfg.pan_min, cfg.pan_max));
    m_tilt.reset(clampf(kHomeTilt, cfg.tilt_min, cfg.tilt_max));
    m_configured = true;
  } else {
    target(m_pan.target(), m_tilt.target());  // re-clamp to new limits
  }
}

void CameraHead::speed(float dps) {
  m_pan.setLimits(dps, m_cfg.accel_dps2);
  m_tilt.setLimits(dps, m_cfg.accel_dps2);
}

void CameraHead::target(float pan, float tilt) {
  m_pan.setTarget(clampf(pan, m_cfg.pan_min, m_cfg.pan_max));
  m_tilt.setTarget(clampf(tilt, m_cfg.tilt_min, m_cfg.tilt_max));
  m_idle = false;
}

void CameraHead::joy(int8_t x, int8_t y, bool active, uint32_t now_ms) {
  if (!active) {
    if (m_joy_active) {
      m_joy_active = false;
      m_return_pending = !m_locked;
      m_release_ms = now_ms;
    }
    return;
  }
  m_joy_active = true;
  m_return_pending = false;
  m_last_joy_ms = now_ms;
  speed(m_cfg.speed_dps);
  const float home_pan = clampf(kHomePan, m_cfg.pan_min, m_cfg.pan_max);
  const float home_tilt = clampf(kHomeTilt, m_cfg.tilt_min, m_cfg.tilt_max);
  const float pan = x >= 0 ? home_pan - x / 100.0f * (home_pan - m_cfg.pan_min)
                           : home_pan - x / 100.0f * (m_cfg.pan_max - home_pan);
  const float tilt = y >= 0 ? home_tilt + y / 100.0f * (m_cfg.tilt_max - home_tilt)
                            : home_tilt + y / 100.0f * (home_tilt - m_cfg.tilt_min);
  target(pan, tilt);
}

void CameraHead::set(float pan_deg, float tilt_deg, float speed_dps) {
  m_joy_active = false;
  m_return_pending = false;
  speed(speed_dps > 0.0f ? speed_dps : m_cfg.speed_dps);
  target(pan_deg, tilt_deg);
}

void CameraHead::nudge(float dpan_deg, float dtilt_deg) {
  m_return_pending = false;
  speed(m_cfg.speed_dps);
  target(m_pan.target() + dpan_deg, m_tilt.target() + dtilt_deg);
}

void CameraHead::home() {
  m_joy_active = false;
  m_return_pending = false;
  speed(m_cfg.speed_dps);
  target(kHomePan, kHomeTilt);
}

void CameraHead::tick(float dt_s, uint32_t now_ms) {
  if (m_joy_active && elapsed(now_ms, m_last_joy_ms, kJoyTimeoutMs)) joy(0, 0, false, m_last_joy_ms + kJoyTimeoutMs);
  if (m_return_pending && elapsed(now_ms, m_release_ms, m_cfg.return_delay_ms)) {
    m_return_pending = false;
    target(kHomePan, kHomeTilt);
  }
  m_pan.step(dt_s);
  m_tilt.step(dt_s);
  const bool mv = moving();
  if (mv) {
    m_idle = false;
  } else if (m_was_moving) {
    m_still_since_ms = now_ms;
  } else if (elapsed(now_ms, m_still_since_ms, kDetachAfterMs)) {
    m_idle = true;
  }
  m_was_moving = mv;
}

}  // namespace cq
