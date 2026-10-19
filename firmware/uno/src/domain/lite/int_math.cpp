#include "domain/lite/int_math.h"

namespace cq {

uint16_t isqrt16(uint16_t n) {
  uint16_t r = 0;
  for (uint16_t bit = 1u << 14; bit != 0; bit >>= 2) {
    if (n >= r + bit) {
      n = static_cast<uint16_t>(n - (r + bit));
      r = static_cast<uint16_t>((r >> 1) + bit);
    } else {
      r >>= 1;
    }
  }
  return r;
}

uint16_t ihypot16(int16_t a, int16_t b) {
  uint16_t ua = static_cast<uint16_t>(a < 0 ? -a : a), ub = static_cast<uint16_t>(b < 0 ? -b : b);
  uint8_t shift = 0;
  while (ua > 180u || ub > 180u) {  // 180² + 180² < 2^16
    ua >>= 1;
    ub >>= 1;
    ++shift;
  }
  return static_cast<uint16_t>(isqrt16(static_cast<uint16_t>(ua * ua + ub * ub)) << shift);
}

int16_t iatan2Cdeg(int16_t y, int16_t x) {
  if (x == 0 && y == 0) return 0;
  const uint16_t ax = static_cast<uint16_t>(x < 0 ? -x : x), ay = static_cast<uint16_t>(y < 0 ? -y : y);
  const bool swap = ay > ax;
  uint16_t num = swap ? ax : ay, den = swap ? ay : ax;
  while (den > 0xFFu) {  // keep the ratio; (num << 8) / den then fits 16 bits
    num >>= 1;
    den >>= 1;
  }
  const uint16_t r = static_cast<uint16_t>((num << 8) / den);  // tan in Q8, 0..256
  // atan(r) ≈ 45 r + 15.64 r (1 - r) degrees (max error 0.22°): 4500/256 = 17 + 37/64, 1564/256 = 6 + 28/256
  const uint16_t q = static_cast<uint16_t>((r * (256u - r)) >> 8);  // 0..64
  int16_t a = static_cast<int16_t>(r * 17u + ((r * 37u) >> 6) + q * 6u + ((q * 28u) >> 8));  // 0..4500
  if (swap) a = static_cast<int16_t>(9000 - a);
  if (x < 0) a = static_cast<int16_t>(18000 - a);
  return y < 0 ? static_cast<int16_t>(-a) : a;
}

}  // namespace cq
