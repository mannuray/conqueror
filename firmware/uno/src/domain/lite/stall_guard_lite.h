#pragma once
#include <stdint.h>

namespace cq {

// Spec 02 §F1.2 / LLD uno-lite §19.1: a driven side is stalled when its speed stays below 30 % of the speed expected
// for its PWM for 0.5 s (replaces "no edge at all": a held track still creeps at 960 counts / wheel turn). A side that
// reads 0 while the tank clearly drives straight (other side moving, both PWMs same sign, |yaw| < 10 °/s) is a
// broken encoder instead (fault, once, checks off for that side). The driven-and-slow time adds up: it pauses while a
// side is not driven (the heading hold may flip its PWM through the deadband, floor test 2026-10-17) and resets only
// when the side moves properly or sits at PWM 0 for 200 ms.
class StallGuardLite {
 public:
  struct Inputs {
    int16_t pwm_l;  // applied last tick
    int16_t pwm_r;
    uint8_t keep_l;  // deadband keep PWM for the current direction
    uint8_t keep_r;
    uint16_t cps_l;
    uint16_t cps_r;
    int16_t yaw_rate_dps;
    bool enabled;  // encoders_enabled
  };
  enum class Result : uint8_t { kNone, kStall, kFault };
  static constexpr uint16_t kMaxCps = 4000;  // counts/s at PWM 255 (phase 1 constant, design-notes §59)
  static constexpr uint8_t kMargin = 15;
  static constexpr uint8_t kSlowPct = 30;  // owner 2026-10-17 (was 15: a held, slipping track took 3-4 s)
  static constexpr uint16_t kDetectMs = 500;
  Result update(const Inputs& in, uint32_t now_ms, int8_t& side);
  bool faulted(uint8_t s) const { return (m_faulted >> s) & 1u; }
  static constexpr uint16_t kIdleResetMs = 200;
  void reset() { m_acc[0] = m_acc[1] = m_idle[0] = m_idle[1] = 0; }

 private:
  uint16_t m_acc[2] = {0, 0};   // driven-and-slow time, ms
  uint16_t m_idle[2] = {0, 0};  // time at PWM 0, ms
  uint8_t m_faulted = 0;
  bool m_started = false;
  uint32_t m_last_ms = 0;
};

}  // namespace cq
