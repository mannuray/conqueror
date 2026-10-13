#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace battery_adc {
void begin();
void start();                 // starts one conversion on A3 (no busy wait)
bool ready(uint16_t& adc);    // true once the conversion finished
}  // namespace battery_adc
}  // namespace port
}  // namespace cq
