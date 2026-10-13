#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {
namespace port {
namespace imu {
bool begin();               // WHO_AM_I check + configuration; false = not present
bool read(ImuRaw& out);     // one 14-byte burst (~380 µs at 400 kHz), mapped to the tank frame
uint16_t i2cErrors();
}  // namespace imu
}  // namespace port
}  // namespace cq
