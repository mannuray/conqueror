#pragma once
#include <stdint.h>

namespace cq {

// Heading from the Z gyro, integers only. IMU at 200 Hz, ±500 °/s = 65.5 raw per °/s, so one degree is
// 65.5 × 200 = 13100 raw·samples and a full turn kTurn = 4716000. yaw [cdeg] = acc / 131. CCW positive.
class YawLite {
 public:
  static constexpr int32_t kTurn = 4716000;
  static constexpr int16_t kStillRaw = 33;  // 0.5 °/s: ignored while the tank is not driven (full: same clamp)
  void update(int16_t gz_corrected, bool stationary);
  int32_t yawCdeg() const { return m_acc / 131; }
  int16_t rateDdps() const { return m_rate_ddps; }
  void reset() { m_acc = 0; }

 private:
  int32_t m_acc = 0;  // 0 .. kTurn-1
  int16_t m_rate_ddps = 0;
};

}  // namespace cq
