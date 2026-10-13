#include "domain/estimation/attitude.h"

#include <math.h>

namespace cq {

namespace {
constexpr float kRadToDeg = 57.29578f;
}

void Attitude::update(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float dt_s) {
  integrateGyro(gx_dps, gy_dps, dt_s);
  if (++m_div >= kAccelEvery) {
    m_div = 0;
    correctAccel(ax_g, ay_g, az_g);
  }
}

void Attitude::integrateGyro(float gx_dps, float gy_dps, float dt_s) {
  // gyro y (left axis) positive = nose down in a right-handed x-forward/z-up frame
  m_pitch_deg -= gy_dps * dt_s;
  m_roll_deg += gx_dps * dt_s;
}

void Attitude::correctAccel(float ax_g, float ay_g, float az_g) {
  // Specific force: +1 g up when level, so nose up gives ax = +sin(pitch).
  const float acc_pitch = atan2f(ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * kRadToDeg;
  const float acc_roll = atan2f(ay_g, az_g) * kRadToDeg;
  m_pitch_deg = kAlpha * m_pitch_deg + (1.0f - kAlpha) * acc_pitch;
  m_roll_deg = kAlpha * m_roll_deg + (1.0f - kAlpha) * acc_roll;
}

}  // namespace cq
