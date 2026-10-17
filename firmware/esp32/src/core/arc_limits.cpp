#include "core/arc_limits.h"

#include <math.h>
#include <stdio.h>

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
double pctFor(double v_cm_s) { return (v_cm_s - motion::kTrackSpeedOffsetCmS) / motion::kTrackSpeedSlopeCmS; }
}  // namespace

ArcBand arcBand(uint16_t radius_cm) {
  ArcBand b;
  if (radius_cm == 0) return b;
  const double k = motion::kTrackWidthCm / 2.0 / radius_cm;
  if (k >= 1.0) return b;  // inside B/2: the inner track would have to reverse
  double lo = ceil(pctFor(motion::kTrackVMinCmS / (1.0 - k)));
  double hi = floor(pctFor(motion::kTrackVMaxCmS / (1.0 + k)));
  if (lo < motion::kTurnWMin) lo = motion::kTurnWMin;
  if (hi > 100.0) hi = 100.0;
  if (lo > hi) return b;
  b.possible = true;
  b.lo = static_cast<uint8_t>(lo);
  b.hi = static_cast<uint8_t>(hi);
  return b;
}

uint16_t arcMinRadiusCm() {
  for (uint16_t r = 1; r < 1000; ++r) {
    if (arcBand(r).possible) return r;
  }
  return 1000;
}

size_t arcRefusal(char* out, size_t cap, uint16_t radius_cm) {
  const ArcBand b = arcBand(radius_cm);
  const int n = b.possible ? snprintf(out, cap, "radius %u cm: use speed %u-%u %%, or pivot",
                                      static_cast<unsigned>(radius_cm), static_cast<unsigned>(b.lo),
                                      static_cast<unsigned>(b.hi))
                           : snprintf(out, cap, "radius %u cm too tight (min %u): use pivot",
                                      static_cast<unsigned>(radius_cm), static_cast<unsigned>(arcMinRadiusCm()));
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

}  // namespace core
}  // namespace cq
