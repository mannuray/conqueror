#pragma once
#include <stdint.h>

namespace cq {
namespace lite_telemetry {

// TELEM_SLOW.flags2 safety-state bits, as the full firmware sets them (app/comms.cpp): brake off, envelope active,
// link lost, deadman latched. Other bits of `base` (battery, self-test lock) are kept.
uint32_t slowFlags(uint32_t base, bool brake_on, bool envelope_active, bool link_alive, bool deadman_latched);
// TELEM_SLOW.hw (HwBit): encoders enabled, IMU ok, ultrasonic ok.
uint8_t hwByte(bool encoders_enabled, bool imu_ok, bool us_ok);

}  // namespace lite_telemetry
}  // namespace cq
