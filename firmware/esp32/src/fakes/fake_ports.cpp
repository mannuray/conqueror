#include "fakes/fake_ports.h"

#include <stdarg.h>
#include <stdio.h>

#include "ports/app_store.h"
#include "ports/camera_control.h"
#include "ports/client_out.h"
#include "ports/log_out.h"
#include "ports/motion_store.h"
#include "ports/system_control.h"
#include "ports/wifi_control.h"

namespace cq {
namespace fakes {

World& world() {
  static World w;
  return w;
}
void reset() { world() = World{}; }
std::string lastTo(int fd) {
  for (auto it = world().sent.rbegin(); it != world().sent.rend(); ++it) {
    if (it->fd == fd) return it->json;
  }
  return "";
}
size_t countTo(int fd, const char* needle) {
  size_t n = 0;
  for (const Sent& s : world().sent) n += (s.fd == fd && s.json.find(needle) != std::string::npos) ? 1 : 0;
  return n;
}

}  // namespace fakes

namespace ports {
using fakes::world;

bool clientSend(int fd, const char* json, size_t len) {
  if (!world().send_ok) return false;
  world().sent.push_back({fd, std::string(json, len)});
  return true;
}
bool clientClose(int fd) {
  world().closed.push_back(fd);
  return world().close_ok;
}
void logLine(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  world().logs.emplace_back(line);
}
uint16_t loadCpm() { return world().cpm; }
bool saveCpm(uint16_t v) {
  if (!world().save_ok) return false;
  world().saved_cpm.push_back(v);
  world().cpm = v;
  return true;
}
uint16_t loadRoll() { return world().roll; }
bool saveRoll(uint16_t v) {
  if (!world().save_ok) return false;
  world().saved_roll.push_back(v);
  world().roll = v;
  return true;
}
uint16_t loadCoast() { return world().coast; }
bool saveCoast(uint16_t v) {
  if (!world().save_ok) return false;
  world().saved_coast.push_back(v);
  world().coast = v;
  return true;
}
bool loadAiUrl(char* out, size_t cap) {
  snprintf(out, cap, "%s", world().ai_url.c_str());
  return true;
}
bool saveAiUrl(const char* url) {
  if (!world().save_ok) return false;
  world().ai_url = url;
  return true;
}
bool wifiPost(const core::WifiRequest& r) {
  if (!world().wifi_post_ok) return false;
  world().wifi_posted.push_back(r);
  return true;
}
bool wifiPoll(core::WifiResult& out) {
  if (world().wifi_results.empty()) return false;
  out = world().wifi_results.front();
  world().wifi_results.pop_front();
  return true;
}
void wifiSnapshot(core::WifiSnapshot& out) { out = world().wifi; }
bool cameraPost(const core::CamSettings& s) {
  if (!world().cam_ok) return false;
  world().cam_posted.push_back(s);
  return true;
}
core::CamSettings cameraSettings() { return world().cam; }
core::CamSettings cameraDefaults() { return world().cam_defaults; }
core::CamStats cameraStats(uint32_t) { return world().cam_stats; }
uint8_t cameraViewers() { return world().viewers; }
void systemRestart() { ++world().restarts; }
uint8_t resetReason() { return world().reset_reason; }

}  // namespace ports
}  // namespace cq
