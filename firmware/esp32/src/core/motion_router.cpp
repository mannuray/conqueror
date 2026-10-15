#include "core/motion_router.h"

#include "cq_protocol.h"

namespace cq {
namespace core {
namespace {
bool has(uint8_t caps, proto::CapBit b) { return (caps & (1u << static_cast<uint8_t>(b))) != 0; }
}  // namespace

Route routeTurn(uint8_t body_caps, uint16_t radius_cm) {
  if (radius_cm > 0) return has(body_caps, proto::CapBit::kMove) ? Route::kForward : Route::kUnavailable;
  return has(body_caps, proto::CapBit::kTurn) ? Route::kForward : Route::kLocal;
}

Route routeDriveRate(uint8_t body_caps) {
  return has(body_caps, proto::CapBit::kDriveRate) ? Route::kForward : Route::kLocal;
}

Route routeMove(uint8_t body_caps) {
  return has(body_caps, proto::CapBit::kMove) ? Route::kForward : Route::kUnavailable;
}

}  // namespace core
}  // namespace cq
