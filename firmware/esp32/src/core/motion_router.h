// Who runs a measured move (spec 02 §E2, bridge LLD R4.1): the body when its HELLO caps report it, else the
// bridge's motion service; distance / arcs without encoders are unavailable.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

enum class Route : uint8_t { kForward, kLocal, kUnavailable };

Route routeTurn(uint8_t body_caps, uint16_t radius_cm, bool pivot = false);  // arc / pivot: body's MOVE, else the bridge with encoders (Plan L)
Route routeDriveRate(uint8_t body_caps);
Route routeMove(uint8_t body_caps);  // MOVE: body's own, else the bridge with the body's encoders (Plan K)

}  // namespace core
}  // namespace cq
