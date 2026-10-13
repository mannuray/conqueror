#pragma once
#include <stdint.h>

#include "domain/motion/motion_setpoint.h"

namespace cq {

struct MoveParams {
  float dist_mm;       // signed (negative = reverse)
  float max_v_pct;
  float v_full_mm_s;   // track speed at 100 %
  float accel_mm_s2;
  float heading_deg;   // held during the move
  float counts_per_mm;
};

// Trapezoidal speed profile on the mean encoder distance; done within 5 mm or on overshoot.
class MoveProfile {
 public:
  static constexpr float kToleranceMm = 5.0f;
  static constexpr float kMinSpeedMmS = 30.0f;
  void start(const MoveParams& p, int32_t enc_l, int32_t enc_r, uint32_t now_ms);
  ProfileState tick(int32_t enc_l, int32_t enc_r, bool stalled, float dt_s, uint32_t now_ms, MotionSetpoint& sp);
  uint8_t progressPct() const { return m_progress; }

 private:
  MoveParams m_p{0, 0, 1, 1, 0, 1};
  int32_t m_l0 = 0;
  int32_t m_r0 = 0;
  float m_v_cmd = 0.0f;
  uint32_t m_start_ms = 0;
  uint32_t m_timeout_ms = 0;
  uint8_t m_progress = 0;
};

}  // namespace cq
