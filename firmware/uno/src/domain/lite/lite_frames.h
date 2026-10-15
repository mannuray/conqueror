#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace lite_frames {

enum class FrameUse : uint8_t { kHandled, kUnavailable, kUnknown };
// Spec 02 Part D / Uno Lite LLD §5: which ESP32 → Uno frames the Lite firmware implements.
FrameUse frameUse(uint8_t type);
// ACK rules as the full firmware (DRIVE, OWNER_HINT, CAM_JOY, link frames: no ACK); every unavailable frame is acked.
bool needsAck(uint8_t type);
uint8_t helloCaps(bool encoders_enabled);

}  // namespace lite_frames
}  // namespace cq
