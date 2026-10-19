// The ESP32 versions of the ports (bridge LLD R15.3): thin forwards to net/. Host builds link fakes/ instead.
#include <Arduino.h>
#include <esp_system.h>
#include <stdarg.h>

#include "net/app_settings.h"
#include "net/camera_service.h"
#include "net/http_server.h"
#include "net/log.h"
#include "net/motion_store.h"
#include "net/stream_server.h"
#include "net/wifi_manager.h"
#include "ports/app_store.h"
#include "ports/camera_control.h"
#include "ports/client_out.h"
#include "ports/log_out.h"
#include "ports/motion_store.h"
#include "ports/system_control.h"
#include "ports/wifi_control.h"

namespace cq {
namespace ports {

bool clientSend(int fd, const char* json, size_t len) { return net::wsSend(fd, json, len); }
bool clientClose(int fd) { return net::wsClose(fd); }

void logLine(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  net::vlogf(fmt, ap);
  va_end(ap);
}

uint16_t loadCpm() { return net::motionStoreLoad(); }
bool saveCpm(uint16_t v) { return net::motionStoreSave(v); }
uint16_t loadRoll() { return net::motionStoreLoadRoll(); }
bool saveRoll(uint16_t v) { return net::motionStoreSaveRoll(v); }

bool loadAiUrl(char* out, size_t cap) { return net::appAiUrlLoad(out, cap); }
bool saveAiUrl(const char* url) { return net::appAiUrlSave(url); }

bool wifiPost(const core::WifiRequest& r) { return net::wifiPost(r); }
bool wifiPoll(core::WifiResult& out) { return net::wifiPoll(out); }
void wifiSnapshot(core::WifiSnapshot& out) { net::wifiSnapshot(out); }

bool cameraPost(const core::CamSettings& s) { return net::cameraPost(s); }
core::CamSettings cameraSettings() { return net::cameraSettings(); }
core::CamSettings cameraDefaults() { return net::cameraDefaults(); }
core::CamStats cameraStats(uint32_t now_ms) { return net::cameraStats(now_ms); }
uint8_t cameraViewers() { return net::streamViewers(); }

void systemRestart() { ESP.restart(); }
uint8_t resetReason() { return static_cast<uint8_t>(esp_reset_reason()); }

}  // namespace ports
}  // namespace cq
