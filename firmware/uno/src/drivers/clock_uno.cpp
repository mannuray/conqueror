#include <Arduino.h>

#include "ports/clock.h"

namespace cq {
namespace port {
namespace clock {
uint32_t nowUs() { return micros(); }
uint32_t nowMs() { return millis(); }
}  // namespace clock
}  // namespace port
}  // namespace cq
