// esp32-camera on the ESP32-S3-EYE pin map (design-notes §24): JPEG VGA, 2 frame buffers in PSRAM.
#pragma once

#include "core/cam_settings.h"

#include <esp_camera.h>

namespace cq {
namespace net {

bool cameraBegin();              // false: no camera (the bridge works without it)
const char* cameraSensorName();  // "OV3660" (expected), other sensor name, or "none"
camera_fb_t* cameraGrab();
void cameraRelease(camera_fb_t* fb);
void cameraStop();  // OTA start: streams end, no more grabs
bool cameraStopped();
bool cameraReady();  // started and not stopped (OTA): port 81 also runs without a camera (R3.11)

using CamSettings = core::CamSettings;  // value types live in core (bridge LLD R15)
using CamStats = core::CamStats;
bool cameraPost(const CamSettings& s);  // any task: queued, applied and saved by cameraApplyPending()
void cameraApplyPending();             // Arduino loop: applies the latest posted settings, saves NVS
CamSettings cameraSettings();          // the settings in force
CamSettings cameraDefaults();
CamStats cameraStats(uint32_t now_ms);  // over the last ~2 s of streamed frames (link task only)
void cameraCountFrame(uint32_t bytes); // stream workers, per sent frame

}  // namespace net
}  // namespace cq
