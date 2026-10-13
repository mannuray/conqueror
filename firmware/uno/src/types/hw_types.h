#pragma once
#include <stdint.h>

namespace cq {

struct EncoderSnapshot {
  int32_t count_l;
  int32_t count_r;
  uint32_t period_l_us;     // time between the last two rising edges (0 = none yet)
  uint32_t period_r_us;
  uint32_t last_edge_l_us;
  uint32_t last_edge_r_us;
  uint32_t t_us;
};

struct ImuRaw {  // tank frame (x forward, y left, z up) after the board axis map
  int16_t ax, ay, az;
  int16_t gx, gy, gz;
  int16_t temp;
  uint32_t t_us;
};

struct IrEvent {
  uint32_t code;
  bool repeat;
  uint32_t t_ms;
};

struct Rgb {
  uint8_t r, g, b;
};

inline bool operator==(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(const Rgb& a, const Rgb& b) { return !(a == b); }

enum class EchoState : uint8_t { kPending, kReady, kClear, kNoEcho };
enum class Axis : uint8_t { kPan = 0, kTilt = 1 };

}  // namespace cq
