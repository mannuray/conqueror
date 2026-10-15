#pragma once
#include <stdint.h>

namespace cq {

// Camera pan/tilt in deci-degrees. pan()/tilt() are the targets; step() moves the servo output toward them at
// most kStepDd per 20 ms tick (120 °/s, smooth motion = LLD §8 extra 1).
// Pan 0..1800 (x > 0 = right = lower angle), tilt 300..1100 (y > 0 = up = lower angle: this tank looks down at
// larger angles, design-notes §45), home 900/900. Joystick release → home after 500 ms
// unless locked.
class CamLite {
 public:
  static constexpr int16_t kHome = 900;
  static constexpr int16_t kPanMin = 0, kPanMax = 1800, kTiltMin = 300, kTiltMax = 1100;
  static constexpr uint16_t kReturnMs = 500;
  static constexpr int16_t kStepDd = 24;
  static constexpr uint16_t kJoyTimeoutMs = 500;   // stick held without refresh = phone lost (full CameraHead)
  static constexpr uint16_t kDetachAfterMs = 2000;  // servos released after 2 s still (spec 02 §D5.4)
  static constexpr int16_t kJogDd = 12;  // per 20 ms tick = 60 °/s (full firmware: 1.2° per tick)
  void set(int16_t pan_dd, int16_t tilt_dd);
  void nudge(int16_t dpan_dd, int16_t dtilt_dd);  // sums in int32: a large nudge saturates, never wraps
  // IR jog keys held (4/6 pan, 2/8 tilt): direction kept until released; applyJog() every 20 ms = 60 °/s.
  void jog(int8_t pan_dir, int8_t tilt_dir) {
    m_jog_pan = pan_dir;
    m_jog_tilt = tilt_dir;
  }
  void applyJog() {
    if (m_jog_pan != 0 || m_jog_tilt != 0) nudge(static_cast<int16_t>(-m_jog_pan * kJogDd), static_cast<int16_t>(-m_jog_tilt * kJogDd));  // up = smaller tilt
  }
  void joy(int8_t x, int8_t y, bool active, uint32_t now_ms);
  void home() { set(kHome, kHome); }
  void lock(bool on) { m_locked = on; }
  void tick(uint32_t now_ms);  // joystick timeout, return home, stillness
  bool wantAttached() const { return !m_idle; }
  int16_t pan() const { return m_pan; }
  int16_t tilt() const { return m_tilt; }
  void step();
  int16_t outPan() const { return m_out_pan; }
  int16_t outTilt() const { return m_out_tilt; }
  static uint16_t toUs(int16_t dd) { return static_cast<uint16_t>(544 + static_cast<int32_t>(dd) * 1856 / 1800); }

 private:
  int16_t m_pan = kHome;
  int16_t m_tilt = kHome;
  int16_t m_out_pan = kHome;
  int16_t m_out_tilt = kHome;
  int8_t m_jog_pan = 0;
  int8_t m_jog_tilt = 0;
  bool m_locked = false;
  bool m_joy_active = false;
  bool m_idle = false;
  bool m_active = false;  // a command since the last tick (wakes the servos even if the move already finished)
  uint32_t m_still_since_ms = 0;
  bool m_return_pending = false;
  uint32_t m_joy_ms = 0;  // last joystick refresh while held, release time while a return is pending (RAM)
};

}  // namespace cq
