#pragma once
#include <stdint.h>

namespace cq {

// Complementary filter for pitch (nose up +) and roll (right side down +), tank frame.
// update() runs at 200 Hz: the gyro is integrated every call, the accelerometer correction
// (atan2f x2 + sqrtf, the expensive part on AVR) every kAccelEvery-th call = 50 Hz (LLD §9.13).
class Attitude {
 public:
  static constexpr uint8_t kAccelEvery = 4;
  static constexpr float kAlpha = 0.92f;  // per 50 Hz correction (~0.98^4: ~0.25 s time constant)
  void reset(float pitch_deg, float roll_deg) {
    m_pitch_deg = pitch_deg;
    m_roll_deg = roll_deg;
    m_div = 0;
  }
  void update(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float dt_s);
  void integrateGyro(float gx_dps, float gy_dps, float dt_s);
  void correctAccel(float ax_g, float ay_g, float az_g);
  float pitchDeg() const { return m_pitch_deg; }
  float rollDeg() const { return m_roll_deg; }

 private:
  float m_pitch_deg = 0.0f;
  float m_roll_deg = 0.0f;
  uint8_t m_div = 0;
};

}  // namespace cq
