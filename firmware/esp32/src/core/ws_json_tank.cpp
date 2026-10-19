#include "core/ws_json.h"
#include "core/ws_json_util.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {
using namespace detail;

const char* unoEventName(uint8_t kind) {
  static const char* const kNames[] = {"unknown", "bump", "lifted", "landed", "tilted", "level", "stall", "blocked",
                                       "unblocked", "control_changed", "ir_override", "deadman_stop", "link_lost",
                                       "link_restored", "batt_low", "batt_critical", "primitive_done", "selftest",
                                       "envelope_changed", "encoder_fault", "imu_fault", "us_fault", "settings_saved",
                                       "settings_reset", "cal_request", "button_press", "rejected", "safety_override",
                                       "heading_fault"};
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(proto::EventKind::kHeadingFault) + 1,
                "a Uno event without a name - add it here (design-notes §97 fix 4)");
  return kind < sizeof(kNames) / sizeof(kNames[0]) ? kNames[kind] : "unknown";
}

size_t jsonTank(char* out, size_t cap, const TankView& v) {
  static const char* const kSizes[] = {"QVGA", "VGA", "SVGA", "HD"};
  char url[200];
  jsonEscape(url, sizeof(url), v.ai_url != nullptr ? v.ai_url : "");
  return fmt(out, cap,
             "{\"t\":\"tank\",\"selftest\":{\"seen\":%s,\"mask\":%u,\"locked\":%s,\"age_s\":%lu},"
             "\"hw\":{\"encoders\":%s,\"imu\":%s,\"us\":%s},"
             "\"camera\":{\"size\":\"%s\",\"quality\":%u,\"fps_cap\":%u,\"brightness\":%d,\"contrast\":%d,"
             "\"low_light\":%s,\"vflip\":%s,\"hmirror\":%s,\"viewers\":%u,\"fps\":%.1f,\"kbps\":%u},"
             "\"ai_url\":\"%s\"}",
             tf(v.st.seen), v.st.mask, tf(v.st.locked), static_cast<unsigned long>(v.st_age_s), tf(v.hw & 1u),
             tf(v.hw & 2u), tf(v.hw & 4u), kSizes[v.cam_size < 4 ? v.cam_size : 1], v.cam_quality, v.cam_fps,
             v.cam_bright, v.cam_contrast, tf(v.cam_low), tf(v.cam_vflip), tf(v.cam_hmirror), v.viewers,
             v.fps_x10 / 10.0, v.kbps, url);
}

namespace {

const char* const kLogKinds[] = {"boot", "uno", "primitive", "stop_hold", "client_lost", "body_online",
                                 "body_offline", "wifi_mode", "restart", "ota"};
static_assert(sizeof(kLogKinds) / sizeof(kLogKinds[0]) == static_cast<size_t>(LogKind::kOta) + 1, "a LogKind without a name");

// One entry as JSON (with a leading comma unless first); its length, or 0 when it does not fit `cap`.
size_t logItem(char* out, size_t cap, const LogEntry& x, bool first, uint32_t now_ms) {
  const uint8_t k = static_cast<uint8_t>(x.kind);
  char ev[48] = "";
  if (x.kind == LogKind::kUno) snprintf(ev, sizeof(ev), ",\"ev\":\"%s\"", unoEventName(static_cast<uint8_t>(x.a)));
  char client[32];
  jsonEscape(client, sizeof(client), x.client);
  return fmt(out, cap, "%s{\"seq\":%lu,\"age_s\":%lu,\"kind\":\"%s\"%s,\"a\":%ld,\"b\":%ld,\"client\":\"%s\"}",
             first ? "" : ",", static_cast<unsigned long>(x.seq), static_cast<unsigned long>((now_ms - x.t_ms) / 1000u),
             k < sizeof(kLogKinds) / sizeof(kLogKinds[0]) ? kLogKinds[k] : "unknown", ev, static_cast<long>(x.a),
             static_cast<long>(x.b), client);
}

}  // namespace

size_t jsonLog(char* out, size_t cap, const LogEntry* e, uint8_t n, uint32_t last, bool gap, uint32_t now_ms) {
  // design-notes §97 fix 2: the closing part always fits; entries go in whole, oldest first, and stop at the first that
  // does not fit (the page asks again from the last seq it got - a hole in the middle would be skipped for good).
  char tail[48];
  const size_t tl = fmt(tail, sizeof(tail), "],\"last\":%lu%s}", static_cast<unsigned long>(last), gap ? ",\"gap\":true" : "");
  size_t len = fmt(out, cap, "{\"t\":\"log\",\"entries\":[");
  if (len == 0 || tl == 0) return 0;
  for (uint8_t i = 0; i < n; ++i) {
    char item[192];
    const size_t il = logItem(item, sizeof(item), e[i], i == 0, now_ms);
    if (il == 0 || len + il + tl >= cap) break;
    memcpy(out + len, item, il);
    len += il;
  }
  memcpy(out + len, tail, tl + 1);
  return len + tl;
}

}  // namespace core
}  // namespace cq
