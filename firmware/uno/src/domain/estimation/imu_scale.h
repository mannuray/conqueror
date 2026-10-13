#pragma once
#include <stdint.h>

namespace cq {
namespace imu_scale {
constexpr float kAccelLsbPerG = 8192.0f;  // ±4 g
constexpr float kGyroLsbPerDps = 65.5f;   // ±500 °/s
// MPU6050: T = raw / 340 + 36.53 °C → returned in 0.01 °C
inline int16_t tempCentiC(int16_t raw) { return static_cast<int16_t>((static_cast<int32_t>(raw) * 100) / 340 + 3653); }
// WHO_AM_I values with the same accel/gyro register map (review item 7): MPU-6050 0x68, MPU-6500 0x70
// (common clone on "MPU6050" boards), MPU-9250 0x71. Temperature scaling differs on the 65xx/92xx
// parts, so their temp telemetry reads a few °C off — accepted.
inline bool supportedWhoAmI(uint8_t id) { return id == 0x68 || id == 0x70 || id == 0x71; }
}  // namespace imu_scale
}  // namespace cq
