#pragma once
#include "types/hw_types.h"

namespace cq {
namespace port {
namespace led {
void begin();
void write(const Rgb& c);  // one WS2812; interrupts off ≈ 32 µs
}  // namespace led
}  // namespace port
}  // namespace cq
