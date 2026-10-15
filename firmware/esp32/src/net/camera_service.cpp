#include "net/camera_service.h"

#include <Arduino.h>

#include <atomic>

#include "board_config.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {
bool g_ok = false;
const char* g_name = "none";
std::atomic<bool> g_stopped{false};

// Per-sensor orientation (ESP32 LLD: sensor PID at boot → profile). The stock OV3660 is mounted so its raw image is
// flipped vertically (looks upside down and back-to-front): vflip only. vflip + hmirror (as ELEGOO's firmware) left
// the image mirrored on the owner's tank (2026-10-15). Other sensors keep the driver defaults until tested.
void applySensorDefaults(sensor_t* s) {
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    logf("camera: OV3660 orientation: vflip");
  }
}
}  // namespace

bool cameraBegin() {
  camera_config_t c{};
  c.pin_pwdn = board::kCamPwdn;
  c.pin_reset = board::kCamReset;
  c.pin_xclk = board::kCamXclk;
  c.pin_sccb_sda = board::kCamSda;
  c.pin_sccb_scl = board::kCamScl;
  c.pin_d7 = board::kCamD7;
  c.pin_d6 = board::kCamD6;
  c.pin_d5 = board::kCamD5;
  c.pin_d4 = board::kCamD4;
  c.pin_d3 = board::kCamD3;
  c.pin_d2 = board::kCamD2;
  c.pin_d1 = board::kCamD1;
  c.pin_d0 = board::kCamD0;
  c.pin_vsync = board::kCamVsync;
  c.pin_href = board::kCamHref;
  c.pin_pclk = board::kCamPclk;
  c.xclk_freq_hz = static_cast<int>(board::kCamXclkHz);
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;  // viewers always get the newest frame
  const esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    logf("camera: init failed 0x%x - stream disabled (check the ribbon cable)",
         static_cast<unsigned>(err));
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    const camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id);
    g_name = info != nullptr ? info->name : "unknown";
    applySensorDefaults(s);
  }
  g_ok = true;
  logf("camera: %s, VGA JPEG q12, 2 frame buffers in PSRAM", g_name);
  return true;
}

const char* cameraSensorName() { return g_name; }

camera_fb_t* cameraGrab() { return (g_ok && !g_stopped) ? esp_camera_fb_get() : nullptr; }

void cameraRelease(camera_fb_t* fb) {
  if (fb != nullptr) esp_camera_fb_return(fb);
}

void cameraStop() { g_stopped = true; }

bool cameraStopped() { return g_stopped; }

}  // namespace net
}  // namespace cq
