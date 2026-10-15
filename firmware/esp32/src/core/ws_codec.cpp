#include "core/ws_codec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return WsCmdKind::kUnsupported;
}

// proto::SoundId names in lower snake case, in enum order (STARTUP = 1 ... GOODBYE = 23); spec 02 §A9 catalogue.
// The page has the same list (web/basic_drive.html SOUNDS); the count is checked against the generated enum.
const char* const kSoundNames[] = {"startup", "ready", "selftest_fail", "chirp_up", "chirp_down", "blip",
                                   "tick", "jingle", "thinking", "ouch", "error", "success",
                                   "fanfare", "sad", "safety_chirp", "ack_blip", "fail", "confirm",
                                   "question", "ok_chirp", "grumble", "scan", "goodbye"};
static_assert(sizeof(kSoundNames) / sizeof(kSoundNames[0]) == static_cast<size_t>(proto::SoundId::kGoodbye),
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
  } else if (by) {
    mf.bad = n < -720.0 || n > 720.0 || lround(n * 100.0) == 0;
    c.turn_by_cdeg = static_cast<int32_t>(lround(n * 100.0));
    mf.by = true;
  } else if (to) {
    mf.bad = n < 0.0 || n >= 360.0;
    c.turn_to_cdeg = static_cast<uint16_t>(lround(n * 100.0) % 36000);
    c.turn_has_to = true;
    mf.to = true;
  } else if (speed) {
    mf.bad = n < 10.0 || n > 100.0 || !isWhole(n);
    c.speed = static_cast<uint8_t>(n);
  } else if (radius) {
    mf.bad = n < 0.0 || n > 1000.0 || !isWhole(n);
    c.radius_cm = static_cast<uint16_t>(n);
  } else {
    mf.bad = n < -180.0 || n > 180.0;
    c.rate_ddps = static_cast<int16_t>(lround(n * 10.0));
    mf.rate = true;
  }
  return true;
}

}  // namespace

WsCommand parseWsCommand(const char* text, size_t len) {
  WsCommand c;
  WsCommand bad;
  Reader r(text, len);
  char t[24] = "";
  bool have_v = false, have_w = false, have_x = false, have_y = false, have_active = false, have_on = false;
  char mode[8] = "";  // audio_mode
  MotionFields mf;
  char name[24] = "";
  if (!r.eat('{')) return bad;
  if (!r.peek('}')) {
    do {
      char key[16];
      if (!r.str(key, sizeof(key)) || !r.eat(':')) return bad;
      double num = 0;
      if (strcmp(key, "t") == 0) {
        if (!r.str(t, sizeof(t))) return bad;
      } else if (strcmp(key, "id") == 0) {
        // Review M4: a non-negative integer that fits int32 (no UB cast, no silent "no id").
        if (!r.num(num, nullptr, 0) || num < 0 || num > 2147483647.0 || num != floor(num)) return bad;
        c.has_id = true;
        c.id = static_cast<int32_t>(num);
      } else if (strcmp(key, "v") == 0 || strcmp(key, "w") == 0 || strcmp(key, "x") == 0 ||
                 strcmp(key, "y") == 0) {
        if (!r.num(num, nullptr, 0)) return bad;
        const int8_t p = clampPct(num);
        switch (key[0]) {
          case 'v':
            c.v = p;
            have_v = true;
            break;
          case 'w':
            c.w = p;
            have_w = true;
            break;
          case 'x':
            c.x = p;
            have_x = true;
            break;
          default:
            c.y = p;
            have_y = true;
            break;
        }
      } else if (strcmp(key, "active") == 0) {
        if (r.word("true")) {
          c.active = true;
        } else if (!r.word("false")) {
          return bad;
        }
        have_active = true;
      } else if (strcmp(key, "on") == 0) {
        if (r.word("true")) {
          c.on = true;
        } else if (!r.word("false")) {
          return bad;
        }
        have_on = true;
      } else if (readMotionField(key, r, c, mf)) {
        if (mf.bad) return bad;
      } else if (strcmp(key, "mode") == 0) {
        if (!r.str(mode, sizeof(mode))) return bad;
      } else if (strcmp(key, "name") == 0) {
        if (!r.str(name, sizeof(name))) return bad;
      } else if (strcmp(key, "t0") == 0) {
        if (!r.num(num, c.t0, sizeof(c.t0))) return bad;
      } else if (strcmp(key, "client") == 0) {
        if (!r.str(c.client, sizeof(c.client))) return bad;
      } else if (!r.skip()) {
        return bad;
      }
    } while (r.eat(','));
  }
  if (!r.eat('}') || !r.atEnd() || t[0] == '\0') return bad;
  c.kind = kindOf(t);
  if (c.kind == WsCmdKind::kDrive && !(have_v && have_w)) return bad;
  if (c.kind == WsCmdKind::kCamJoy && !(have_x && have_y && have_active)) return bad;
  if (c.kind == WsCmdKind::kPing && c.t0[0] == '\0') snprintf(c.t0, sizeof(c.t0), "null");
  if (c.kind == WsCmdKind::kSound) {
    c.sound_id = soundIdOf(name);  // missing or unknown name -> 0 -> invalid (never an out-of-range SoundId)
    if (c.sound_id == 0) return bad;
  }
  if (c.kind == WsCmdKind::kAudioMode && !audioModeOf(mode, c.audio_mode)) return bad;
  if (c.kind == WsCmdKind::kCamLock && !have_on) return bad;
  if (c.kind == WsCmdKind::kTurn && mf.by == mf.to) return bad;  // exactly one of by / to
  if (c.kind == WsCmdKind::kDriveRate && !(have_v && mf.rate)) return bad;  // review M20: like the other commands' fields
  return c;
}

}  // namespace core
}  // namespace cq
