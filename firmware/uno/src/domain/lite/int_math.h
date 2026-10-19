// Uno Lite LLD §21: integer maths for tilt angles and the stick distance (no floats are linked on the Lite).
// 16-bit arithmetic only: 32-bit division / multiply / shift loops cost several hundred bytes on the AVR.
#pragma once

#include <stdint.h>

namespace cq {

uint16_t isqrt16(uint16_t n);                // floor(sqrt(n))
uint16_t ihypot16(int16_t a, int16_t b);     // sqrt(a² + b²), |a|, |b| <= 16383 (scaled down first when large)
int16_t iatan2Cdeg(int16_t y, int16_t x);    // 0.01°, -18000..18000, error <= 0.5°

}  // namespace cq
