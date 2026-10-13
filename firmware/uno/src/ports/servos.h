#pragma once
#include "types/hw_types.h"

namespace cq {
namespace port {
namespace servos {
void begin();
void attach(Axis a);
void detach(Axis a);
void writeDeg(Axis a, float deg);  // 0..180 → 544..2400 µs
}  // namespace servos
}  // namespace port
}  // namespace cq
