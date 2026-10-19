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
bool cameraReady();  // started and not stopped (OTA): port 81 also runs without a camera (R3.11)

// Plan O (bridge LLD R13): settings changed at run time, saved in NVS cq_cam. size 0 QVGA, 1 VGA, 2 SVGA, 3 HD;
// fps_cap 0 = no limit. The camera starts at HD so its frame buffers fit every size.
struct CamSettings {
  uint8_t size = 1;
  uint8_t quality = 12;
  uint8_t fps_cap = 0;
  int8_t brightness = 0;
  int8_t contrast = 0;
  bool low_light = false;
  bool vflip = true;
  bool hmirror = false;
};
bool cameraPost(const CamSettings& s);  // any task: queued, applied and saved by cameraApplyPending()
void cameraApplyPending();             // Arduino loop: applies the latest posted settings, saves NVS
CamSettings cameraSettings();          // the settings in force
CamSettings cameraDefaults();
struct CamStats {
  uint16_t fps_x10 = 0;
  uint16_t kbps = 0;
};
CamStats cameraStats(uint32_t now_ms);  // over the last ~2 s of streamed frames (link task only)
void cameraCountFrame(uint32_t bytes); // stream workers, per sent frame

}  // namespace net
}  // namespace cq
