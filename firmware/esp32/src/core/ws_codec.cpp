#include "core/ws_codec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/motion_config.h"
#include "cq_protocol.h"

namespace cq {
namespace core {
namespace {

class Reader {
 public:
  Reader(const char* s, size_t n) : m_s(s), m_end(s + n) {}

  void ws() {
    while (m_s < m_end && (*m_s == ' ' || *m_s == '\t' || *m_s == '\r' || *m_s == '\n')) ++m_s;
  }
  bool eat(char c) {
    ws();
    if (m_s < m_end && *m_s == c) {
      ++m_s;
      return true;
    }
    return false;
  }
  bool peek(char c) {
    ws();
    return m_s < m_end && *m_s == c;
  }
  // Reads a string into out (truncated to cap-1); escapes are kept only for \" and \\.
  bool str(char* out, size_t cap) {
    if (!eat('"')) return false;
    size_t n = 0;
    while (m_s < m_end && *m_s != '"') {
      char c = *m_s++;
      if (c == '\\') {
        if (m_s >= m_end) return false;
        c = *m_s++;
      }
      if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = '\0';
    return eat('"');
  }
  // Reads a number; raw (for t0) gets it re-formatted as a valid JSON number (review M5).
  bool num(double& value, char* raw, size_t cap) {
    ws();
    const char* start = m_s;
    while (m_s < m_end && (strchr("+-.eE", *m_s) != nullptr || (*m_s >= '0' && *m_s <= '9'))) ++m_s;
    const size_t n = static_cast<size_t>(m_s - start);
    if (n == 0 || n >= 24) return false;
    char tmp[24];
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    char* endp = nullptr;
    value = strtod(tmp, &endp);
    if (endp != tmp + n || !isfinite(value)) return false;
    if (raw && cap > 0) snprintf(raw, cap, "%.15g", value);
    return true;
  }
  bool word(const char* w) {
    ws();
    const size_t n = strlen(w);
    if (static_cast<size_t>(m_end - m_s) < n || strncmp(m_s, w, n) != 0) return false;
    m_s += n;
    return true;
  }
  // Skips any value, including nested objects/arrays.
  bool skip() {
    ws();
    if (m_s >= m_end) return false;
    if (*m_s == '"') {
      char dummy[2];
      return str(dummy, sizeof(dummy));
    }
    if (*m_s == '{' || *m_s == '[') {
      int depth = 0;
      bool in_str = false;
      while (m_s < m_end) {
        const char c = *m_s++;
        if (in_str) {
          if (c == '\\') {
            ++m_s;
          } else if (c == '"') {
            in_str = false;
          }
        } else if (c == '"') {
          in_str = true;
        } else if (c == '{' || c == '[') {
          ++depth;
        } else if (c == '}' || c == ']') {
          if (--depth == 0) return true;
        }
      }
      return false;
    }
    if (word("true") || word("false") || word("null")) return true;
    double v = 0;
    return num(v, nullptr, 0);
  }
  bool atEnd() {
    ws();
    return m_s == m_end;
  }

 private:
  const char* m_s;
  const char* m_end;
};

int8_t clampPct(double v) {
  if (v > 100.0) return 100;
  if (v < -100.0) return -100;
  return static_cast<int8_t>(v < 0 ? v - 0.5 : v + 0.5);
}

WsCmdKind kindOf(const char* t) {
  if (strcmp(t, "drive") == 0) return WsCmdKind::kDrive;
  if (strcmp(t, "stop") == 0) return WsCmdKind::kStop;
  if (strcmp(t, "cam_joy") == 0) return WsCmdKind::kCamJoy;
  if (strcmp(t, "cam_home") == 0) return WsCmdKind::kCamHome;
  if (strcmp(t, "release_control") == 0) return WsCmdKind::kRelease;
  if (strcmp(t, "ping") == 0) return WsCmdKind::kPing;
  if (strcmp(t, "hello") == 0) return WsCmdKind::kHello;
  if (strcmp(t, "sound") == 0) return WsCmdKind::kSound;
  if (strcmp(t, "cam_lock") == 0) return WsCmdKind::kCamLock;
  if (strcmp(t, "audio_mode") == 0) return WsCmdKind::kAudioMode;
  if (strcmp(t, "turn") == 0) return WsCmdKind::kTurn;
  if (strcmp(t, "drive_rate") == 0) return WsCmdKind::kDriveRate;
  if (strcmp(t, "cancel") == 0) return WsCmdKind::kCancel;
  if (strcmp(t, "debug") == 0) return WsCmdKind::kDebug;
  if (strcmp(t, "move") == 0) return WsCmdKind::kMove;
  if (strcmp(t, "calib") == 0) return WsCmdKind::kCalib;
  if (strcmp(t, "wifi_status") == 0) return WsCmdKind::kWifiStatus;
  if (strcmp(t, "wifi_scan") == 0) return WsCmdKind::kWifiScan;
  if (strcmp(t, "wifi_save") == 0) return WsCmdKind::kWifiSave;
  if (strcmp(t, "wifi_forget") == 0) return WsCmdKind::kWifiForget;
  if (strcmp(t, "wifi_mode") == 0) return WsCmdKind::kWifiMode;
  if (strcmp(t, "ap_password") == 0) return WsCmdKind::kApPassword;
  if (strcmp(t, "setting") == 0) return WsCmdKind::kSetting;
  if (strcmp(t, "restart") == 0) return WsCmdKind::kRestart;
  return WsCmdKind::kUnsupported;
}

// proto::SoundId names in lower snake case, in enum order (STARTUP = 1 ... HORN = 24); spec 02 §A9 catalogue.
// The page has the same list (web/basic_drive.html SOUNDS); the count is checked against the generated enum.
const char* const kSoundNames[] = {"startup", "ready", "selftest_fail", "chirp_up", "chirp_down", "blip",
                                   "tick", "jingle", "thinking", "ouch", "error", "success",
                                   "fanfare", "sad", "safety_chirp", "ack_blip", "fail", "confirm",
                                   "question", "ok_chirp", "grumble", "scan", "goodbye", "horn"};
static_assert(sizeof(kSoundNames) / sizeof(kSoundNames[0]) == static_cast<size_t>(proto::SoundId::kHorn),
              "kSoundNames must list every proto::SoundId in order");

uint8_t soundIdOf(const char* name) {
  for (uint8_t i = 0; i < sizeof(kSoundNames) / sizeof(kSoundNames[0]); ++i) {
    if (strcmp(name, kSoundNames[i]) == 0) return static_cast<uint8_t>(i + 1);
  }
  return 0;
}

// audio_mode name -> proto::AudioModeValue; false for a missing or unknown name.
bool audioModeOf(const char* name, uint8_t& out) {
  using M = proto::AudioModeValue;
  const M m = strcmp(name, "normal") == 0 ? M::kNormal : strcmp(name, "quiet") == 0 ? M::kQuiet : M::kMute;
  if (m == M::kMute && strcmp(name, "mute") != 0) return false;
  out = static_cast<uint8_t>(m);
  return true;
}

// Plan H: the motion fields (turn by / to / speed / radius_cm, drive_rate yaw_rate) - ranges per LLD R4.2.
struct MotionFields {
  bool by = false;
  bool to = false;
  bool rate = false;
  bool bad = false;
};

bool isWhole(double n) { return n == floor(n); }

// True when `key` is a motion field (its value is consumed); a bad value sets mf.bad.
bool readMotionField(const char* key, Reader& r, WsCommand& c, MotionFields& mf) {
  const bool by = strcmp(key, "by") == 0, to = strcmp(key, "to") == 0, speed = strcmp(key, "speed") == 0;
  const bool radius = strcmp(key, "radius_cm") == 0, rate = strcmp(key, "yaw_rate") == 0;
  if (!(by || to || speed || radius || rate)) return false;
  double n = 0;
  if (!r.num(n, nullptr, 0)) {
    mf.bad = true;
  } else if (by) {  // ±1..720° (LLD R4.2)
    mf.bad = fabs(n) < 1.0 || fabs(n) > 720.0;
    if (!mf.bad) c.turn_by_cdeg = static_cast<int32_t>(lround(n * 100.0));
    mf.by = true;
  } else if (to) {
    mf.bad = n < 0.0 || n >= 360.0;
    if (!mf.bad) c.turn_to_cdeg = static_cast<uint16_t>(lround(n * 100.0) % 36000);
    c.turn_has_to = true;
    mf.to = true;
  } else if (speed) {  // 1..100 here; the per-command minimum (turn w_min - review M1, move 10 %) is in complete()
    mf.bad = n < 1.0 || n > 100.0 || !isWhole(n);
    if (!mf.bad) c.speed = static_cast<uint8_t>(n);
  } else if (radius) {
    mf.bad = n < 0.0 || n > 1000.0 || !isWhole(n);
    if (!mf.bad) c.radius_cm = static_cast<uint16_t>(n);
  } else {
    mf.bad = n < -180.0 || n > 180.0;
    if (!mf.bad) c.rate_ddps = static_cast<int16_t>(lround(n * 10.0));
    mf.rate = true;
  }
  return true;
}

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
};

bool readBool(Reader& r, bool& out, bool& seen) {
  if (r.word("true")) {
    out = true;
  } else if (!r.word("false")) {
    return false;
  }
  seen = true;
  return true;
}

// One "key": value pair; false = malformed (the whole command is rejected).
bool readField(const char* key, Reader& r, WsCommand& c, Seen& s) {
  double num = 0;
  if (strcmp(key, "t") == 0) return r.str(s.t, sizeof(s.t));
  if (strcmp(key, "id") == 0) {
    // Review M4: a non-negative integer that fits int32 (no UB cast, no silent "no id").
    if (!r.num(num, nullptr, 0) || num < 0 || num > 2147483647.0 || num != floor(num)) return false;
    c.has_id = true;
    c.id = static_cast<int32_t>(num);
    return true;
  }
  if (key[0] != '\0' && key[1] == '\0' && strchr("vwxy", key[0]) != nullptr) {
    if (!r.num(num, nullptr, 0)) return false;
    const int8_t p = clampPct(num);
    switch (key[0]) {
      case 'v': c.v = p; s.v = true; break;
      case 'w': c.w = p; s.w = true; break;
      case 'x': c.x = p; s.x = true; break;
      default: c.y = p; s.y = true; break;
    }
    return true;
  }
  if (strcmp(key, "active") == 0) return readBool(r, c.active, s.active);
  if (strcmp(key, "on") == 0) return readBool(r, c.on, s.on);
  if (strcmp(key, "pivot") == 0) return readBool(r, c.pivot, s.pivot);
  if (readMotionField(key, r, c, s.mf)) return !s.mf.bad;
  if (strcmp(key, "mode") == 0) return r.str(s.mode, sizeof(s.mode));
  if (strcmp(key, "name") == 0) return r.str(s.name, sizeof(s.name));
  if (strcmp(key, "t0") == 0) return r.num(num, c.t0, sizeof(c.t0));
  if (strcmp(key, "client") == 0) return r.str(c.client, sizeof(c.client));
  if (strcmp(key, "hz") == 0) {  // debug: 10 or 50 only (LLD R5)
    if (!r.num(num, nullptr, 0) || (num != 10.0 && num != 50.0)) return false;
    c.debug_hz = static_cast<uint8_t>(num);
    return true;
  }
  if (strcmp(key, "dist_cm") == 0) {  // move: whole cm, 1..500 in size
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num == 0.0 || fabs(num) > motion::kMoveMaxCm) return false;
    c.move_cm = static_cast<int16_t>(num);
    s.dist = true;
    return true;
  }
  if (strcmp(key, "heading") == 0) {  // move heading: not in Plan K (spec 02 §G1) - refused in complete()
    s.heading = true;
    return r.skip();
  }
  if (strcmp(key, "ssid") == 0) {  // Plan M: 1..32 bytes (checked again by WifiStore)
    s.ssid = r.str(c.ssid, sizeof(c.ssid));
    return s.ssid;
  }
  if (strcmp(key, "password") == 0) {
    s.password = r.str(c.password, sizeof(c.password));
    return s.password;
  }
  if (strcmp(key, "slot") == 0) {  // JSON 1..3
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < 1 || num > 3) return false;
    c.slot = static_cast<int8_t>(num - 1);
    return true;
  }
  if (strcmp(key, "value") == 0) {  // setting
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < -2147483648.0 || num > 2147483647.0) return false;
    c.value = static_cast<int32_t>(num);
    c.has_value = true;
    return true;
  }
  if (strcmp(key, "reset") == 0) return r.str(s.reset, sizeof(s.reset));
  if (strcmp(key, "setting_id") == 0) {  // setting: the Uno setting (the command id stays "id")
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < 0 || num > 255) return false;
    c.setting_id = static_cast<uint8_t>(num);
    s.setting_id = true;
    return true;
  }
  if (strcmp(key, "counts_per_m") == 0) {  // calib set
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < motion::kCpmMin || num > motion::kCpmMax) return false;
    c.has_cpm = true;
    c.cpm = static_cast<uint16_t>(num);
    return true;
  }
  return r.skip();
}

// The fields each command needs; fills the derived values. False = invalid command.
bool complete(WsCommand& c, const Seen& s) {
  c.kind = kindOf(s.t);
  switch (c.kind) {
    case WsCmdKind::kDrive: return s.v && s.w;
    case WsCmdKind::kCamJoy: return s.x && s.y && s.active;
    case WsCmdKind::kPing:
      if (c.t0[0] == '\0') snprintf(c.t0, sizeof(c.t0), "null");
      return true;
    case WsCmdKind::kSound:
      c.sound_id = soundIdOf(s.name);  // missing or unknown name -> 0 -> invalid (never an out-of-range SoundId)
      return c.sound_id != 0;
    case WsCmdKind::kAudioMode: return audioModeOf(s.mode, c.audio_mode);
    case WsCmdKind::kCamLock: return s.on;
    case WsCmdKind::kTurn:  // exactly one of by / to; a pivot has no radius (Plan L)
      return s.mf.by != s.mf.to && c.speed >= motion::kTurnWMin && !(c.pivot && c.radius_cm > 0);
    case WsCmdKind::kMove: return s.dist && !s.heading && c.speed >= motion::kMoveSpeedMin;
    case WsCmdKind::kWifiSave: return s.ssid && s.password;
    case WsCmdKind::kWifiForget: return true;
    case WsCmdKind::kWifiMode:
      c.wifi_ap = strcmp(s.mode, "ap") == 0;
      return c.wifi_ap || strcmp(s.mode, "home") == 0;
    case WsCmdKind::kApPassword: return s.password;
    case WsCmdKind::kSetting: return s.setting_id;
    case WsCmdKind::kCalib:
      if (s.reset[0] == '\0') return true;
      c.calib_reset = strcmp(s.reset, "counts_per_m") == 0 ? 0 : (strcmp(s.reset, "roll") == 0 ? 1 : -1);
      return c.calib_reset >= 0;
    case WsCmdKind::kWifiStatus:
    case WsCmdKind::kWifiScan:
    case WsCmdKind::kRestart: return true;
    case WsCmdKind::kDriveRate: return s.v && s.mf.rate;  // review M20: like the other commands' fields
    case WsCmdKind::kDebug: return s.on;
    default: return true;
  }
}

}  // namespace

WsCommand parseWsCommand(const char* text, size_t len) {
  WsCommand c;
  const WsCommand bad;
  Reader r(text, len);
  Seen s;
  if (!r.eat('{')) return bad;
  if (!r.peek('}')) {
    do {
      char key[16];
      if (!r.str(key, sizeof(key)) || !r.eat(':') || !readField(key, r, c, s)) return bad;
    } while (r.eat(','));
  }
  if (!r.eat('}') || !r.atEnd() || s.t[0] == '\0') return bad;
  return complete(c, s) ? c : bad;
}

}  // namespace core
}  // namespace cq
