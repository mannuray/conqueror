#include "net/camera_service.h"

#include <Arduino.h>
#include <string.h>

#include <atomic>

#include <Preferences.h>

#include <driver/gpio.h>

#include "board_config.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {
bool g_ok = false;
CamSettings g_cur;                    // in force (spinlock)
portMUX_TYPE g_cam_mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t g_cam_q = nullptr;      // latest posted settings (depth 1, overwritten)
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_bytes{0};

framesize_t frameSizeOf(uint8_t s) {
  const framesize_t k[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_HD};
  return k[s < 4 ? s : 1];
}

void applyTo(sensor_t* s, const CamSettings& c) {
  if (s == nullptr) return;
  s->set_framesize(s, frameSizeOf(c.size));
  s->set_quality(s, c.quality);
  s->set_brightness(s, c.brightness);
  s->set_contrast(s, c.contrast);
  // design-notes §96: low light = night mode (longer exposures in the dark) + 32x gain ceiling + one step brighter
  s->set_aec2(s, c.low_light ? 1 : 0);
  s->set_gainceiling(s, c.low_light ? GAINCEILING_32X : GAINCEILING_4X);
  s->set_ae_level(s, c.low_light ? 1 : 0);
  s->set_vflip(s, c.vflip ? 1 : 0);
  s->set_hmirror(s, c.hmirror ? 1 : 0);
}

bool loadSettings(CamSettings& c) {
  Preferences p;
  if (!p.begin("cq_cam", true)) return false;
  const bool ok = p.getBytesLength("s") == sizeof(CamSettings) && p.getBytes("s", &c, sizeof(c)) == sizeof(c);
  p.end();
  return ok && c.size < 4 && c.quality >= 10 && c.quality <= 40;
}

void saveSettings(const CamSettings& c) {
  Preferences p;
  if (!p.begin("cq_cam", false)) {
    logf("camera: settings not saved (NVS open failed)");
    return;
  }
  if (p.putBytes("s", &c, sizeof(c)) != sizeof(c)) logf("camera: settings not saved (NVS write failed)");
  p.end();
}
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

namespace {

camera_config_t cameraConfig() {  // ESP32-S3-EYE pin map (design-notes §24)
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
  c.frame_size = FRAMESIZE_HD;  // Plan O: buffers sized for the largest choice; the saved size is set below
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;  // viewers always get the newest frame
  return c;
}

void gentleClock() {  // LLD R12: gentle clock edges, also after a failed init (it can leave the clock running)
  gpio_set_drive_capability(static_cast<gpio_num_t>(board::kCamXclk), static_cast<gpio_drive_cap_t>(board::kCamXclkDrive));
  gpio_drive_cap_t drive = GPIO_DRIVE_CAP_DEFAULT;
  gpio_get_drive_capability(static_cast<gpio_num_t>(board::kCamXclk), &drive);
  logf("camera: xclk GPIO%d drive %d", board::kCamXclk, static_cast<int>(drive));
}

}  // namespace

bool cameraBegin() {
  const camera_config_t c = cameraConfig();
  const esp_err_t err = esp_camera_init(&c);
  gentleClock();
  if (err != ESP_OK) {
    logf("camera: init failed 0x%x - stream disabled (check the ribbon cable)", static_cast<unsigned>(err));
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    const camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id);
    g_name = info != nullptr ? info->name : "unknown";
    applySensorDefaults(s);
  }
  CamSettings cs = cameraDefaults();
  if (!loadSettings(cs)) cs = cameraDefaults();
  applyTo(s, cs);
  portENTER_CRITICAL(&g_cam_mux);
  g_cur = cs;
  portEXIT_CRITICAL(&g_cam_mux);
  if (g_cam_q == nullptr) g_cam_q = xQueueCreate(1, sizeof(CamSettings));
  g_ok = true;
  logf("camera: %s, size %u q%u (buffers for HD), 2 frame buffers in PSRAM", g_name, cs.size, cs.quality);
  return true;
}

const char* cameraSensorName() { return g_name; }

camera_fb_t* cameraGrab() { return (g_ok && !g_stopped) ? esp_camera_fb_get() : nullptr; }

void cameraRelease(camera_fb_t* fb) {
  if (fb != nullptr) esp_camera_fb_return(fb);
}

void cameraStop() { g_stopped = true; }

bool cameraStopped() { return g_stopped; }

bool cameraReady() { return g_ok && !g_stopped; }

CamSettings cameraDefaults() {
  CamSettings c;
  c.vflip = g_name != nullptr && strcmp(g_name, "OV3660") == 0;  // the stock OV3660 is mounted upside down
  return c;
}

bool cameraPost(const CamSettings& s) { return g_cam_q != nullptr && xQueueOverwrite(g_cam_q, &s) == pdTRUE; }

void cameraApplyPending() {
  CamSettings c;
  if (g_cam_q == nullptr || !g_ok || g_stopped || xQueueReceive(g_cam_q, &c, 0) != pdTRUE) return;
  applyTo(esp_camera_sensor_get(), c);
  portENTER_CRITICAL(&g_cam_mux);
  g_cur = c;
  portEXIT_CRITICAL(&g_cam_mux);
  saveSettings(c);
  logf("camera: settings size %u q%u fps %u b%d c%d low %u", c.size, c.quality, c.fps_cap, c.brightness, c.contrast,
       c.low_light ? 1u : 0u);
}

CamSettings cameraSettings() {
  portENTER_CRITICAL(&g_cam_mux);
  const CamSettings c = g_cur;
  portEXIT_CRITICAL(&g_cam_mux);
  return c;
}

void cameraCountFrame(uint32_t bytes) {
  g_frames.fetch_add(1);
  g_bytes.fetch_add(bytes);
}

CamStats cameraStats(uint32_t now_ms) {
  static uint32_t last_ms = 0, last_frames = 0, last_bytes = 0;
  static CamStats cached;
  const uint32_t dt = now_ms - last_ms;
  if (dt >= 2000) {
    const uint32_t f = g_frames.load(), b = g_bytes.load();
    cached.fps_x10 = static_cast<uint16_t>((f - last_frames) * 10000u / dt);
    cached.kbps = static_cast<uint16_t>((b - last_bytes) * 8u / dt);  // bytes * 8 / ms = kbit/s
    last_ms = now_ms;
    last_frames = f;
    last_bytes = b;
  }
  return cached;
}

}  // namespace net
}  // namespace cq
