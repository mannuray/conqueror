// The Tank Logo events (Plan N3, Tank Logo LLD §20.4).
#include "core/ws_json.h"
#include "core/ws_json_util.h"

#include <stdio.h>

namespace cq {
namespace core {
using namespace detail;

size_t jsonLogoStarted(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* name, const char* by) {
  char n[64], b[32];
  jsonEscape(n, sizeof n, name);
  jsonEscape(b, sizeof b, by);
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_started\",\"name\":\"%s\",\"by\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), n, b);
}

size_t jsonLogoLine(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t line, int32_t round) {
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_line\",\"line\":%u,\"round\":%ld}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), line, static_cast<long>(round));
}

size_t jsonLogoPose(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, int32_t x_mm, int32_t y_mm, int16_t heading) {
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_pose\",\"x_cm\":%.1f,\"y_cm\":%.1f,\"heading\":%d}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), x_mm / 10.0, y_mm / 10.0, heading);
}

size_t jsonLogoSay(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* text) {
  char t[256];
  jsonEscape(t, sizeof t, text);
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_say\",\"text\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), t);
}

size_t jsonLogoNote(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t line, const char* msg) {
  char m[200];
  jsonEscape(m, sizeof m, msg);
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_note\",\"line\":%u,\"msg\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), line, m);
}

size_t jsonLogoEnded(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t code, uint16_t line,
                     const char* msg) {
  char m[200], reason[12];
  jsonEscape(m, sizeof m, msg);
  if (code == 0) {
    snprintf(reason, sizeof reason, "\"finished\"");
  } else {
    snprintf(reason, sizeof reason, "%u", code);
  }
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_ended\",\"reason\":%s,\"line\":%u,\"msg\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), reason, line, m);
}

}  // namespace core
}  // namespace cq
