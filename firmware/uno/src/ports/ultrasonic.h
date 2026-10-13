#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {
namespace port {
namespace ultrasonic {
void begin();
bool trigger();                       // 10 µs pulse + arms the echo capture; false (no ping) while echo is high
EchoState poll(uint16_t& echo_us);    // result of the previous trigger; kPending if it sent no ping
}  // namespace ultrasonic
}  // namespace port
}  // namespace cq
