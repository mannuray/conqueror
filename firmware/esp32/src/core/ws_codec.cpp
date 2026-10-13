#include "core/ws_codec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return WsCmdKind::kUnsupported;
}

}  // namespace

WsCommand parseWsCommand(const char* text, size_t len) {
  WsCommand c;
  WsCommand bad;
  Reader r(text, len);
  char t[24] = "";
  bool have_v = false, have_w = false, have_x = false, have_y = false, have_active = false;
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
  return c;
}

}  // namespace core
}  // namespace cq
