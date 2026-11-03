// A point of the run's frame seen from the tank now (Tank Logo LLD §24.2, bridge LLD R26.2): SETXY's place and the
// AREA's anchor go to the skill server relative to the tank, so the engine needs no frame of the run. Pure.
// The run frame is the odometry's (logo/odometry.h): start (0, 0) facing +y, x to the right, heading right = +.
#pragma once

#include <math.h>
#include <stdint.h>

#include "logo/odometry.h"

namespace cq {
namespace logo {

// A place relative to the tank now (cm; forward +, left +). core::FwdLeft is this type (core/ws_json_skill.h).
struct FwdLeft {
  int16_t fwd_cm = 0;
  int16_t left_cm = 0;
};

namespace detail {
inline int16_t cmOf(double mm) {
  const double cm = mm / 10.0;
  if (cm >= 32767.0) return 32767;
  if (cm <= -32767.0) return -32767;
  return static_cast<int16_t>(lround(cm));
}
}  // namespace detail

// The tank faces (sin h, cos h) in the run frame; its left is (-cos h, sin h).
inline FwdLeft relative(const Pose& now, int32_t x_run_mm, int32_t y_run_mm) {
  const double h = now.heading_deg * 3.14159265358979323846 / 180.0;
  const double dx = static_cast<double>(x_run_mm) - now.x_mm;
  const double dy = static_cast<double>(y_run_mm) - now.y_mm;
  FwdLeft r;
  r.fwd_cm = detail::cmOf(dx * sin(h) + dy * cos(h));
  r.left_cm = detail::cmOf(-dx * cos(h) + dy * sin(h));
  return r;
}

}  // namespace logo
}  // namespace cq
