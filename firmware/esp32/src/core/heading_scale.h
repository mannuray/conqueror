// The Uno's gyro heading corrected by one scale (bridge LLD R19): each frame's change times gyro_scale / 10000,
// summed in whole numbers fine enough that the rounding never builds up. Pure, host-tested.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

constexpr uint16_t kGyroScaleDefault = 10000;  // x1.0000: no correction
constexpr uint16_t kGyroScaleMin = 9000;
constexpr uint16_t kGyroScaleMax = 11000;

class HeadingScale {
 public:
  bool setScale(uint16_t scale_e4);  // false (unchanged) outside kGyroScaleMin..kGyroScaleMax
  uint16_t scale() const { return m_scale; }
  void reset() { m_anchored = false; }  // the next frame anchors: corrected = raw
  // yaw_raw 0.01° (0..35999), rate_raw 0.1 °/s in; the corrected pair out.
  void apply(uint16_t yaw_raw, int16_t rate_raw, uint16_t& yaw, int16_t& rate);

 private:
  uint16_t m_scale = kGyroScaleDefault;
  bool m_anchored = false;
  uint16_t m_last_raw = 0;
  int32_t m_sum_e4 = 0;  // corrected heading in 1/10000 of 0.01°, 0 .. 36000 x 10000 - 1
};

}  // namespace core
}  // namespace cq
