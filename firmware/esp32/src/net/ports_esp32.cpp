// The ESP32 versions of the ports (bridge LLD R15.3): thin forwards to net/. Host builds link fakes/ instead.
#include <Arduino.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "net/camera_service.h"
#include "net/http_server.h"
#include "net/log.h"
#include "net/motion_store.h"
#include "net/stream_server.h"
#include "net/wifi_manager.h"
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
bool clientPeerIp(int fd, char* out, size_t cap) {
  sockaddr_storage a = {};
  socklen_t n = sizeof a;
  if (cap == 0 || getpeername(fd, reinterpret_cast<sockaddr*>(&a), &n) != 0) return false;
  if (a.ss_family == AF_INET) {
    return inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&a)->sin_addr, out, cap) != nullptr;
  }
  if (a.ss_family != AF_INET6) return false;
  const uint8_t* b = reinterpret_cast<const sockaddr_in6*>(&a)->sin6_addr.s6_addr;  // ::ffff:a.b.c.d only
  static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  if (memcmp(b, kMapped, sizeof kMapped) != 0) return false;
  snprintf(out, cap, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
  return true;
}

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
uint16_t loadCoast() { return net::motionStoreLoadCoast(); }
bool saveCoast(uint16_t v) { return net::motionStoreSaveCoast(v); }
uint16_t loadGyroScale() { return net::motionStoreLoadGyroScale(); }
bool saveGyroScale(uint16_t v) { return net::motionStoreSaveGyroScale(v); }
uint8_t loadGameCap() { return net::motionStoreLoadGameCap(); }
bool saveGameCap(uint8_t pct) { return net::motionStoreSaveGameCap(pct); }


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
