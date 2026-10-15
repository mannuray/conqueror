// Plan H test support: a simple skid-steer turn model for closed-loop tests of the bridge motion service.
// Turn rate follows w x 2.2 °/s per % (floor data 2026-10-15) with a 150 ms lag; below 8 % the tracks do not
// turn (static friction); commands arrive 40 ms late and telemetry reports 40 ms late (80 ms round trip);
// the reported rate carries +-3 °/s seeded noise. Yaw + = CCW, wrapped 0..360 like the Uno's.
#pragma once

#include <math.h>
#include <stdint.h>

namespace cqtest {

class TankModel {
 public:
  explicit TankModel(double start_yaw_deg, uint32_t seed = 12345) : m_yaw(start_yaw_deg), m_seed(seed) {
    for (int i = 0; i < kDelay; ++i) m_cmd[i] = 0, m_tel_yaw[i] = start_yaw_deg, m_tel_rate[i] = 0;
  }
  // Advance 20 ms with the command `w` (%) just sent by the bridge.
  void step(int8_t w) {
    m_cmd[m_head] = w;
    const int8_t applied = m_cmd[(m_head + 1) % kDelay];  // sent 2 ticks (40 ms) ago
    const double target = (applied > -8 && applied < 8) ? 0.0 : applied * 2.2;
    m_rate += (target - m_rate) * (0.02 / 0.15);
    m_yaw += m_rate * 0.02;
    m_turned += m_rate * 0.02;
    m_tel_yaw[m_head] = m_yaw;
    m_tel_rate[m_head] = m_rate + noise();
    m_head = (m_head + 1) % kDelay;
  }
  uint16_t reportedYawCdeg() const {  // what the Uno reported 40 ms ago, wrapped
    double y = fmod(m_tel_yaw[m_head], 360.0);
    if (y < 0) y += 360.0;
    return static_cast<uint16_t>(lround(y * 100.0)) % 36000;
  }
  int16_t reportedRateDdps() const { return static_cast<int16_t>(lround(m_tel_rate[m_head] * 10.0)); }
  double turnedDeg() const { return m_turned; }
  double rateDps() const { return m_rate; }

 private:
  static constexpr int kDelay = 2;
  double noise() {
    m_seed = m_seed * 1103515245u + 12345u;
    return ((m_seed >> 16) % 601) / 100.0 - 3.0;  // -3.00 .. +3.00
  }
  double m_yaw;
  double m_rate = 0.0;
  double m_turned = 0.0;
  int8_t m_cmd[kDelay];
  double m_tel_yaw[kDelay];
  double m_tel_rate[kDelay];
  int m_head = 0;
  uint32_t m_seed;
};

}  // namespace cqtest
