#pragma once
#include <stdint.h>

namespace cq {

// LLD §9.6. Silent encoder with drive PWM above the deadband for 0.5 s: a latched FAULT when other
// evidence says the track moves (other track ticking + no yaw while driving straight, or a spin seen by
// the gyro with both encoders silent); otherwise a STALL. The supervisor cuts a stalled track; the stall
// clears when the requested direction reverses or after 1 s (then 0.5 s of drive to re-detect).
class EncoderHealth {
 public:
  struct Inputs {
    int16_t pwm_l;  // applied last tick (after the safety gate)
    int16_t pwm_r;
    int16_t req_l;  // asked of the supervisor last tick (a cut track still shows its direction)
    int16_t req_r;
    bool edges_l;
    bool edges_r;
    float yaw_rate_dps;
    uint8_t keep_l;
    uint8_t keep_r;
  };
  static constexpr uint16_t kDetectMs = 500;
  static constexpr uint16_t kStallHoldMs = 1000;
  static constexpr uint8_t kMargin = 15;

  void update(const Inputs& in, uint32_t now_ms);
  bool stalled(uint8_t side) const { return (m_stalled >> side) & 1u; }
  uint8_t stalledMask() const { return m_stalled; }
  bool faulted(uint8_t side) const { return (m_faulted >> side) & 1u; }
  uint8_t takeNewStalls() {
    const uint8_t v = m_new_stalls;
    m_new_stalls = 0;
    return v;
  }
  uint8_t takeNewFaults() {
    const uint8_t v = m_new_faults;
    m_new_faults = 0;
    return v;
  }
  void clearFaults() { m_faulted = 0; }

 private:
  bool m_silent[2] = {false, false};
  uint32_t m_since_ms[2] = {0, 0};
  uint32_t m_stall_ms[2] = {0, 0};
  int8_t m_stall_dir[2] = {0, 0};
  uint8_t m_stalled = 0;
  uint8_t m_faulted = 0;
  uint8_t m_new_stalls = 0;
  uint8_t m_new_faults = 0;
};

}  // namespace cq
