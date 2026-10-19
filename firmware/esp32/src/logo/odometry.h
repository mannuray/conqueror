// The driven path while a program runs (Tank Logo LLD §11, §20.1): dead reckoning from the encoder counts (signed
// by drive direction, counts per metre) and the gyro yaw. Same axes as the planned path: start (0, 0) facing +y,
// x to the right, heading in degrees, right = +. Pure.
#pragma once

#include <stdint.h>

namespace cq {
namespace logo {

struct Pose {
  int32_t x_mm = 0;
  int32_t y_mm = 0;
  int16_t heading_deg = 0;
};

class Odometry {
 public:
  void start(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg, uint16_t counts_per_m);
  Pose update(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg);
  Pose pose() const;

 private:
  double heading(uint16_t yaw_cdeg) const;  // since the start, -180..180, right = +
  int32_t m_l = 0, m_r = 0;
  uint16_t m_yaw0 = 0;
  uint16_t m_cpm = 7514;
  double m_x = 0, m_y = 0, m_h = 0;
};

}  // namespace logo
}  // namespace cq
