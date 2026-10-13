#pragma once
#include <stdint.h>

#include "domain/safety/safety_override.h"
#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// Route handlers (controlInit/controlTask are declared in app/tasks.h).
proto::AckStatus onDrive(const uint8_t* p, uint8_t len);
proto::AckStatus onStop(const uint8_t* p, uint8_t len);
proto::AckStatus onBrake(const uint8_t* p, uint8_t len);
proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len);
proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len);
proto::AckStatus onSafetyOverride(const uint8_t* p, uint8_t len);

// Safety Override glue (LLD §10): the Gate from link/envelope/faults/owner, and the event drain that every
// caller of SafetyOverride runs right after its call (one pending event slot).
SafetyOverride::Gate overrideGate();
void drainOverride();

}  // namespace app
}  // namespace cq
