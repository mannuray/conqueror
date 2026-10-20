#include "logo/odometry.h"

#include <math.h>

namespace cq {
namespace logo {
namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap180(double d) {
  while (d > 180.0) d -= 360.0;
  while (d <= -180.0) d += 360.0;
  return d;
}

}  // namespace

void Odometry::start(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg, uint16_t counts_per_m) {
  m_l = enc_l;
  m_r = enc_r;
  m_yaw0 = yaw_cdeg;
  m_cpm = counts_per_m != 0 ? counts_per_m : 7514;
  m_x = m_y = m_h = 0;
  m_dir_l = m_dir_r = 1;
}

double Odometry::heading(uint16_t yaw_cdeg) const {
  return wrap180(-(static_cast<int32_t>(yaw_cdeg) - static_cast<int32_t>(m_yaw0)) / 100.0);  // tank: left = +
}

// The distance since the last update along the heading half-way between the two readings (good on arcs).
Pose Odometry::update(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg, int8_t cmd_l, int8_t cmd_r) {
  if (cmd_l != 0) m_dir_l = cmd_l > 0 ? 1 : -1;
  if (cmd_r != 0) m_dir_r = cmd_r > 0 ? 1 : -1;
  const double counts = (m_dir_l * (enc_l - m_l) + m_dir_r * (enc_r - m_r)) / 2.0;
  m_l = enc_l;
  m_r = enc_r;
  const double h = heading(yaw_cdeg);
  const double mid = m_h + wrap180(h - m_h) / 2.0;
  const double d_mm = counts * 1000.0 / m_cpm;
  m_x += d_mm * sin(mid * kPi / 180.0);
  m_y += d_mm * cos(mid * kPi / 180.0);
  m_h = h;
  return pose();
}

Pose Odometry::pose() const {
  Pose p;
  p.x_mm = static_cast<int32_t>(lround(m_x));
  p.y_mm = static_cast<int32_t>(lround(m_y));
  p.heading_deg = static_cast<int16_t>(lround(m_h));
  return p;
}

}  // namespace logo
}  // namespace cq
