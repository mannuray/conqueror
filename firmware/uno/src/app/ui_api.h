#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// uiInit/uiTask are declared in app/tasks.h.
proto::AckStatus onLed(const uint8_t* p, uint8_t len);
proto::AckStatus onSound(const uint8_t* p, uint8_t len);
proto::AckStatus onTone(const uint8_t* p, uint8_t len);
proto::AckStatus onAudioMode(const uint8_t* p, uint8_t len);

}  // namespace app
}  // namespace cq
