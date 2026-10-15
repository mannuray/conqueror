// Who runs a measured move (spec 02 §E2, bridge LLD R4.1): the body when its HELLO caps report it, else the
// bridge's motion service; distance / arcs without encoders are unavailable.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

enum class Route : uint8_t { kForward, kLocal, kUnavailable };

Route routeTurn(uint8_t body_caps, uint16_t radius_cm);  // radius > 0 = arc: needs the body's MOVE (encoders)
Route routeDriveRate(uint8_t body_caps);
Route routeMove(uint8_t body_caps);

}  // namespace core
}  // namespace cq
