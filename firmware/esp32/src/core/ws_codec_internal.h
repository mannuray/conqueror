// Internal to core/ws_codec*.cpp (bridge LLD R15.6): what the field readers saw, for the per-command checks.
#pragma once

#include "core/json_reader.h"
#include "core/ws_codec.h"

namespace cq {
namespace core {
namespace codec {

// Plan H: the motion fields seen (turn by / to, drive_rate yaw_rate) - ranges per LLD R4.2.
struct MotionFields {
  bool by = false;
  bool to = false;
  bool rate = false;
};

// What the field loop saw, for the per-command checks after it.
struct Seen {
  char t[24] = "";
  bool v = false, w = false, x = false, y = false, active = false, on = false;
  char mode[8] = "";  // audio_mode / wifi_mode
  char name[24] = "";
  MotionFields mf;
  bool dist = false, heading = false;  // move (Plan K)
  bool pivot = false;                  // turn (Plan L)
  bool ssid = false, password = false, setting_id = false;  // Plan M
  char reset[16] = "";                                      // calib reset
  bool reset_true = false;                                  // camera_set reset (Plan O)
  bool url_bad = false;                                     // app_set
};

// One "key": value pair; false = malformed (the whole command is rejected). Unknown keys are skipped.
bool readField(const char* key, Reader& r, WsCommand& c, Seen& s);

}  // namespace codec
}  // namespace core
}  // namespace cq
