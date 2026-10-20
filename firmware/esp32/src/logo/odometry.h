// The driven path while a program runs (Tank Logo LLD §11, §20.1): dead reckoning from the encoder counts
// (counts per metre) and the gyro yaw. The Uno counts rising edges only - the counts rise forward AND in
// reverse - so each track's direction comes from its motor command (the last non-zero one while it rolls on).
// Same axes as the planned path: start (0, 0) facing +y,
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
  // cmd_l / cmd_r: the tracks' commanded speeds (TELEM_FAST spd_cmd, forward +); 0 keeps the last direction.
  Pose update(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg, int8_t cmd_l = 0, int8_t cmd_r = 0);
  Pose pose() const;

 private:
  double heading(uint16_t yaw_cdeg) const;  // since the start, -180..180, right = +
  int32_t m_l = 0, m_r = 0;
  int8_t m_dir_l = 1, m_dir_r = 1;  // +1 forward, -1 reverse
  uint16_t m_yaw0 = 0;
  uint16_t m_cpm = 7514;
  double m_x = 0, m_y = 0, m_h = 0;
};

}  // namespace logo
}  // namespace cq
