// Port: camera settings and stream stats (bridge LLD R13, R15.3).
#pragma once

#include <stdint.h>

#include "core/cam_settings.h"

namespace cq {
namespace ports {

bool cameraPost(const core::CamSettings& s);  // false: no camera
core::CamSettings cameraSettings();           // in force
core::CamSettings cameraDefaults();
core::CamStats cameraStats(uint32_t now_ms);  // all viewers together
uint8_t cameraViewers();

}  // namespace ports
}  // namespace cq
