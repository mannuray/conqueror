// Host-only port implementations that record every call (bridge LLD R15.11). Linked into env:native only.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "core/cam_settings.h"
#include "core/wifi_types.h"

namespace cq {
namespace fakes {

struct Sent {
  int fd;
  std::string json;
};

struct World {
  std::vector<Sent> sent;
  std::vector<int> closed;
  std::vector<std::string> logs;
  bool send_ok = true;
  bool close_ok = true;
  uint16_t cpm = 7514;
  uint16_t roll = 27;
  uint16_t coast = 60;
  bool save_ok = true;
  std::vector<uint16_t> saved_cpm;
  std::vector<uint16_t> saved_roll;
  std::vector<uint16_t> saved_coast;
  std::string ai_url;
  std::vector<core::WifiRequest> wifi_posted;
  bool wifi_post_ok = true;
  std::deque<core::WifiResult> wifi_results;
  core::WifiSnapshot wifi;
  std::vector<core::CamSettings> cam_posted;
  core::CamSettings cam;
  core::CamSettings cam_defaults;
  core::CamStats cam_stats;
  uint8_t viewers = 0;
  bool cam_ok = true;
  int restarts = 0;
  uint8_t reset_reason = 1;
};

World& world();
void reset();
std::string lastTo(int fd);         // the last json sent to fd ("" if none)
size_t countTo(int fd, const char* needle);  // messages to fd containing needle

}  // namespace fakes
}  // namespace cq
