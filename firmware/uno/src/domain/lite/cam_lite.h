#pragma once
#include <stdint.h>

namespace cq {

// Camera pan/tilt in deci-degrees. pan()/tilt() are the targets; step() moves the servo output toward them at
// most kStepDd per 20 ms tick (120 °/s, smooth motion = LLD §8 extra 1).
// Pan 0..1800 (x > 0 = right = lower angle), tilt 300..1100, home 900/900. Joystick release → home after 500 ms
// unless locked.
class CamLite {
 public:
  static constexpr int16_t kHome = 900;
  static constexpr int16_t kPanMin = 0, kPanMax = 1800, kTiltMin = 300, kTiltMax = 1100;
  static constexpr uint16_t kReturnMs = 500;
  static constexpr int16_t kStepDd = 24;
  void set(int16_t pan_dd, int16_t tilt_dd);
  void nudge(int16_t dpan_dd, int16_t dtilt_dd) { set(static_cast<int16_t>(m_pan + dpan_dd), static_cast<int16_t>(m_tilt + dtilt_dd)); }
  void joy(int8_t x, int8_t y, bool active, uint32_t now_ms);
  void home() { set(kHome, kHome); }
  void lock(bool on) { m_locked = on; }
  void tick(uint32_t now_ms);
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
  bool m_locked = false;
  bool m_return_pending = false;
  uint32_t m_released_ms = 0;
};

}  // namespace cq
