// Plan H test support: a simple skid-steer turn model for closed-loop tests of the bridge motion service.
// Turn rate follows w x 2.2 °/s per % (floor data 2026-10-15) with a 150 ms lag; below 8 % the tracks do not
// turn (static friction); commands arrive 40 ms late and telemetry reports 40 ms late (80 ms round trip);
// the reported rate carries +-3 °/s seeded noise. Yaw + = CCW, wrapped 0..360 like the Uno's.
// R18 (design-notes §112): setRealistic() models the floor instead - the motor deadband lifts the slowest spin
// to ~50 °/s (no 2 °/s crawl), and with w = 0 the tank glides to a stop at a fixed deceleration (momentum).
#pragma once

#include <math.h>
#include <stdint.h>

namespace cqtest {

class TankModel {
 public:
  explicit TankModel(double start_yaw_deg, uint32_t seed = 12345) : m_yaw(start_yaw_deg), m_seed(seed) {
    for (int i = 0; i < kDelay; ++i) m_cmd[i] = 0, m_tel_yaw[i] = start_yaw_deg, m_tel_rate[i] = 0;
  }
  // A body that flips w when reversing (car-style, ignoring src AUTO) - to test the bridge's divergence guard.
  void setCarFlip(bool on) { m_car_flip = on; }
  // R18: |w| < 8 % no turn; from 8 % the spin jumps to 50 °/s, then +1.4 °/s per % (60 % -> 123 °/s); cut to
  // w = 0 it glides at decel_dps2 (°/s²) - 1000 gives ~2° from 65 °/s, plus the 40 ms the command takes to arrive.
  void setRealistic(double decel_dps2 = 1000.0) {
    m_realistic = true;
    m_decel = decel_dps2;
  }
  int directionChanges() const { return m_flips; }  // the tank's real rate changed sign (a wobble)
  int pushes() const { return m_pushes; }            // times the command went from 0 to turning
  // Advance 20 ms with the command (v, w) (%) just sent by the bridge.
  void step(int8_t w, int8_t v = 0) {
    if (m_car_flip && v < 0) w = static_cast<int8_t>(-w);
    m_cmd[m_head] = w;
    const int8_t applied = m_cmd[(m_head + 1) % kDelay];  // sent 2 ticks (40 ms) ago
    if (m_realistic) {
      stepRealistic(applied);
    } else {
      const double target = (applied > -8 && applied < 8) ? 0.0 : applied * 2.2;
      m_rate += (target - m_rate) * (0.02 / 0.15);
    }
    if (w != 0 && m_last_w == 0) ++m_pushes;
    m_last_w = w;
    if (m_rate != 0.0) {
      const int sign = m_rate > 0 ? 1 : -1;
      if (m_last_sign != 0 && sign != m_last_sign) ++m_flips;
      m_last_sign = sign;
    }
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
  void stepRealistic(int8_t applied) {
    const int a = applied < 0 ? -applied : applied;
    if (a < 8) {  // no drive: momentum, a fixed deceleration down to 0 (never past it)
      const double dv = m_decel * 0.02;
      m_rate = fabs(m_rate) <= dv ? 0.0 : m_rate - (m_rate > 0 ? dv : -dv);
      return;
    }
    const double target = (applied < 0 ? -1.0 : 1.0) * (50.0 + 1.4 * (a - 8));
    m_rate += (target - m_rate) * (0.02 / 0.15);
  }
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
  bool m_car_flip = false;
  bool m_realistic = false;
  double m_decel = 1000.0;
  int8_t m_last_w = 0;
  int m_pushes = 0;
  int m_last_sign = 0;
  int m_flips = 0;
};

}  // namespace cqtest
