// Arc radius / speed band (Plan L, spec 02 §H3, bridge LLD R8.3): an arc of radius R at centre speed v needs the
// inner track at v·(1 − B/2R) >= v_min and the outer at v·(1 + B/2R) <= v_max, so each radius has a usable speed
// band (model constants: motion_config.h).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

struct ArcBand {
  bool possible = false;
  uint8_t lo = 0;  // speed %
  uint8_t hi = 0;
};

ArcBand arcBand(uint16_t radius_cm);
uint16_t arcMinRadiusCm();  // smallest radius with a usable band (31 with the H1 model)
// "radius 40 cm: use speed 43-64 %, or pivot" / "radius 20 cm too tight (min 31): use pivot"; 0 when it does not fit.
size_t arcRefusal(char* out, size_t cap, uint16_t radius_cm);

}  // namespace core
}  // namespace cq
