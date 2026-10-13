#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// cameraInit/cameraTask are declared in app/tasks.h.
void configureCamera();  // re-read servo settings (applySettings)

proto::AckStatus onCamJoy(const uint8_t* p, uint8_t len);
proto::AckStatus onCamSet(const uint8_t* p, uint8_t len);
proto::AckStatus onCamNudge(const uint8_t* p, uint8_t len);
proto::AckStatus onCamHome(const uint8_t* p, uint8_t len);
proto::AckStatus onCamLock(const uint8_t* p, uint8_t len);

}  // namespace app
}  // namespace cq
