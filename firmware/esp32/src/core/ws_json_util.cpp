#include "core/ws_json_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/ws_json.h"

namespace cq {
namespace core {
namespace detail {

size_t fmt(char* out, size_t cap, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  const int n = vsnprintf(out, cap, f, ap);
  va_end(ap);
  return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

const char* primName(PrimKind k) {
  if (k == PrimKind::kGo) return "go";  // R20
  return k == PrimKind::kRate ? "rate" : k == PrimKind::kMove ? "move" : "turn";
}

}  // namespace detail

namespace {

// Plan M review I4: only valid UTF-8 goes out (a WebSocket text frame must be UTF-8); a bad sequence -> U+FFFD.
size_t utf8Length(unsigned char c) {  // from the lead byte; 0 = not a valid lead byte
  if (c >= 0xC2 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) return 3;
  return (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
}

size_t utf8Sequence(const char*& s, char* esc) {
  const size_t len = utf8Length(static_cast<unsigned char>(*s));
  bool ok = len != 0;
  for (size_t i = 1; ok && i < len; ++i) {
    const unsigned char d = static_cast<unsigned char>(s[i]);
    ok = d >= 0x80 && d <= 0xBF;  // also stops at the terminator (0x00)
  }
  if (!ok) {
    memcpy(esc, "\xEF\xBF\xBD", 3);
    return 3;
  }
  memcpy(esc, s, len);
  s += len - 1;
  return len;
}

// One input character (or UTF-8 sequence, advancing s) as its JSON text in esc; returns its length.
size_t escapeOne(const char*& s, char* esc) {
  const unsigned char c = static_cast<unsigned char>(*s);
  if (c == '"' || c == '\\') {
    esc[0] = '\\';
    esc[1] = static_cast<char>(c);
    return 2;
  }
  if (c < 0x20) return static_cast<size_t>(snprintf(esc, 7, "\\u%04x", c));
  if (c >= 0x80) return utf8Sequence(s, esc);
  esc[0] = static_cast<char>(c);
  return 1;
}

}  // namespace

size_t jsonEscape(char* out, size_t cap, const char* s) {
  if (cap == 0) return 0;
  size_t n = 0;
  for (; *s != '\0'; ++s) {
    char esc[7];
    const size_t k = escapeOne(s, esc);
    if (n + k + 1 > cap) break;
    memcpy(out + n, esc, k);
    n += k;
  }
  out[n] = '\0';
  return n;
}

}  // namespace core
}  // namespace cq
