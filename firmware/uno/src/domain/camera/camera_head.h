#pragma once
#include <stdint.h>

#include "domain/camera/servo_profile.h"

namespace cq {

struct CameraConfig {
  uint8_t pan_min;
  uint8_t pan_max;
  uint8_t tilt_min;
  uint8_t tilt_max;
  int16_t pan_trim_dd;   // 0.1°
  int16_t tilt_trim_dd;
  uint16_t speed_dps;
  uint16_t accel_dps2;
  uint16_t return_delay_ms;
  bool idle_detach;
};

// Pan/tilt for the ESP32 camera. CAM_JOY is absolute (stick position = angle); on release (or 500 ms
// without CAM_JOY) the head returns home after return_delay unless locked. Pan: x > 0 = right = lower angle.
class CameraHead {
 public:
  static constexpr float kHomePan = 90.0f;
  static constexpr float kHomeTilt = 90.0f;
  static constexpr uint16_t kJoyTimeoutMs = 500;
  static constexpr uint16_t kDetachAfterMs = 2000;

  void configure(const CameraConfig& cfg);
  void joy(int8_t x, int8_t y, bool active, uint32_t now_ms);
  void set(float pan_deg, float tilt_deg, float speed_dps);
  void nudge(float dpan_deg, float dtilt_deg);
  void home();
  void lock(bool on) { m_locked = on; }
  void tick(float dt_s, uint32_t now_ms);
  float panDeg() const { return m_pan.position(); }
  float tiltDeg() const { return m_tilt.position(); }
  float panOutDeg() const { return m_pan.position() + m_cfg.pan_trim_dd * 0.1f; }
  float tiltOutDeg() const { return m_tilt.position() + m_cfg.tilt_trim_dd * 0.1f; }
  bool moving() const { return m_pan.moving() || m_tilt.moving(); }
  bool wantAttached() const { return !m_cfg.idle_detach || !m_idle; }

 private:
  void target(float pan, float tilt);
  void speed(float dps);
  CameraConfig m_cfg{0, 180, 30, 110, 0, 0, 120, 400, 500, false};
  ServoProfile m_pan;
  ServoProfile m_tilt;
  bool m_joy_active = false;
  uint32_t m_last_joy_ms = 0;
  bool m_return_pending = false;
  uint32_t m_release_ms = 0;
  bool m_locked = false;
  bool m_idle = false;
  uint32_t m_still_since_ms = 0;
  bool m_was_moving = false;
  bool m_configured = false;
};

}  // namespace cq
