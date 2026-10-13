// esp32-camera on the ESP32-S3-EYE pin map (design-notes §24): JPEG VGA, 2 frame buffers in PSRAM.
#pragma once

#include <esp_camera.h>

namespace cq {
namespace net {

bool cameraBegin();              // false: no camera (the bridge works without it)
const char* cameraSensorName();  // "OV3660" (expected), other sensor name, or "none"
camera_fb_t* cameraGrab();
void cameraRelease(camera_fb_t* fb);
void cameraStop();  // OTA start: streams end, no more grabs
bool cameraStopped();

}  // namespace net
}  // namespace cq
