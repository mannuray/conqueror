#pragma once
#include <stdint.h>

namespace cq {
namespace imu_scale {
constexpr float kAccelLsbPerG = 8192.0f;  // ±4 g
constexpr float kGyroLsbPerDps = 65.5f;   // ±500 °/s
// MPU6050: T = raw / 340 + 36.53 °C → returned in 0.01 °C
inline int16_t tempCentiC(int16_t raw) { return static_cast<int16_t>((static_cast<int32_t>(raw) * 100) / 340 + 3653); }
}  // namespace imu_scale
}  // namespace cq
