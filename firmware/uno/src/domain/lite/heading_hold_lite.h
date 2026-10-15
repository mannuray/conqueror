#pragma once
#include <stdint.h>

namespace cq {

// kp: % turn per degree × 256; kd: % per °/s × 256.
struct HeadingGains {
  int16_t kp_q8;
  int16_t kd_q8;
};
// Settings are in milli units (full firmware ids 58 pid_heading_kp, 55 pid_rate_kp). 3500 → 1575 (≈ 6.15 %/°,
// ±10 PWM per track per degree at 80 %); 1200 → 307 (1.2 % per °/s).
HeadingGains headingGainsFromMilli(int32_t heading_kp_milli, int32_t rate_kp_milli);

// Floor-tested behaviour (design-notes §41): steering is open-loop; when it stops, wait for |rate| < 5 °/s or
// 0.3 s, capture the heading, then hold it with P on the error and D on the rate. Not scaled by turn authority.
class HeadingHoldLite {
 public:
  static constexpr int16_t kCaptureDdps = 50;
  static constexpr uint8_t kSettleTicks = 30;  // 10 ms control ticks
  void configure(const HeadingGains& g) { m_g = g; }
  // One of w_steer (driver, open loop) and w_ctrl (hold) is set; the other is 0.
  void teleop(int8_t v_pct, int8_t w_pct, bool gyro_ok, int32_t yaw_cdeg, int16_t rate_ddps, int8_t& w_steer,
              int8_t& w_ctrl);
  void reset();
  bool holding() const { return m_state == State::kHolding; }

 private:
  enum class State : uint8_t { kIdle, kSteering, kSettling, kHolding };
  HeadingGains m_g{1575, 307};
  State m_state = State::kIdle;
  int32_t m_held_cdeg = 0;
  uint8_t m_settle = 0;
};

}  // namespace cq
