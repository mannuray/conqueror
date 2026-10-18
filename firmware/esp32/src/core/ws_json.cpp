#include "core/ws_json.h"

#include "core/motion_router.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {
namespace {

struct ReasonInfo {
  uint16_t code;
  const char* reason;
  const char* message;
};

// Index = on-wire ack_status (protocol/defs/errors.yaml, append-only). Codes are checked against
// the generated kAckStatusCode table in test_ws_json.
constexpr ReasonInfo kAckReasons[] = {
    {0, "OK", "OK"},
    {3001, "NOT_OWNER", "Another source has control"},
    {3002, "BLOCKED", "Blocked by a safety rule"},
    {3003, "UNAVAILABLE", "Capability not available"},
    {3004, "ENVELOPE_LOCKED", "Locked by the safety envelope"},
    {3005, "BUSY", "Busy, try again"},
    {3006, "INVALID_COMMAND", "Invalid command"},
    {3013, "CANCELLED", "Cancelled"},
    {3014, "TIMEOUT", "Primitive timed out"},
    {3015, "STALLED", "Track stalled"},
    {3012, "LINK_LOST", "Link to the body lost"},
    {8001, "SETTING_UNKNOWN", "Unknown setting"},
    {8002, "SETTING_OUT_OF_RANGE", "Value out of range"},
    {8003, "SETTINGS_SAVE_BUSY", "Save already in progress"},
    {6001, "CAL_PRECONDITION", "Calibration preconditions not met"},
    {6002, "CAL_FAILED", "Calibration failed"},
    {6003, "CAL_CANCELLED", "Calibration cancelled"},
    {9001, "INTERNAL", "Internal error"},
    {1002, "ROLE_FORBIDDEN", "Role may not do this"},
};
constexpr size_t kAckReasonCount = sizeof(kAckReasons) / sizeof(kAckReasons[0]);
static_assert(static_cast<size_t>(proto::AckStatus::kRoleForbidden) + 1 == kAckReasonCount,
              "ack_status grew: add the new reason to kAckReasons");

constexpr ReasonInfo kBridgeReasons[] = {
    {3001, "NOT_OWNER", "Another source has control"},
    {3005, "BUSY", "Busy, try again"},
    {3007, "BODY_OFFLINE", "Body (Uno) offline"},
    {3010, "RATE_LIMITED", "Too many commands"},
    {3006, "INVALID_COMMAND", "Invalid command"},
    {3009, "COMMAND_TIMEOUT", "No acknowledgement in time"},
    {3003, "UNAVAILABLE", "Capability not available"},  // Plan H: e.g. turn with radius / move without encoders
};

const char* domain(uint16_t code) {
  switch (code / 1000) {
    case 1:
      return "AUTH";
    case 2:
      return "TANK";
    case 3:
      return "COMMAND";
    case 6:
      return "CALIBRATION";
    case 8:
      return "SETTINGS";
    default:
      return "SYSTEM";
  }
}

size_t fmt(char* out, size_t cap, const char* f, ...) __attribute__((format(printf, 3, 4)));
size_t fmt(char* out, size_t cap, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  const int n = vsnprintf(out, cap, f, ap);
  va_end(ap);
  return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

size_t nack(char* out, size_t cap, int32_t id, const ReasonInfo& r) {
  char id_part[24] = "";
  if (id >= 0) snprintf(id_part, sizeof(id_part), "\"id\":%ld,", static_cast<long>(id));
  return fmt(out, cap, "{\"t\":\"nack\",%s\"code\":\"%s_%u\",\"reason\":\"%s\",\"message\":\"%s\"}",
             id_part, domain(r.code), r.code, r.reason, r.message);
}

bool bit(uint32_t v, unsigned b) { return ((v >> b) & 1u) != 0; }

const char* overrideReason(int16_t b) {
  static const char* const kNames[] = {"network", "ir",          "idle",      "max_time",
                                       "stop",    "owner_change", "link_lost", "envelope"};
  return (b >= 0 && b < 8) ? kNames[b] : "unknown";
}

}  // namespace

const char* ownerName(proto::Owner o) {
  switch (o) {
    case proto::Owner::kIr:
      return "ir";
    case proto::Owner::kNetwork:
    case proto::Owner::kCalibration:
      return "human";
    case proto::Owner::kNone:
    default:
      return "none";
  }
}

// HELLO CapBits (Plan F): features the body firmware has; the page hides the rest.
const char* capBit(const HelloInfo& h, proto::CapBit b) {
  return (h.seen && (h.caps & (1u << static_cast<uint8_t>(b)))) ? "true" : "false";
}

// Plan H (spec 02 §E2): turn / drive_rate are available when the bridge's motion service runs them - a body
// without them (Uno Lite). A body that has them is not forwarded to yet (review M7), so false there for now;
// move: run by the bridge when the body has encoders (Plan K).
const char* localOnly(const HelloInfo& h, proto::CapBit b) {
  return (h.seen && !(h.caps & (1u << static_cast<uint8_t>(b)))) ? "true" : "false";
}

// Plan K (LLD R7.1): MOVE is available when the body has its own (as before) or the bridge runs it for a body
// with encoders.
const char* moveCap(const HelloInfo& h) {
  return h.seen && routeMove(h.caps) != Route::kUnavailable ? "true" : "false";
}

size_t jsonWelcome(char* out, size_t cap, const WelcomeInfo& w) {
  char uno[16] = "null";
  if (w.hello->seen) snprintf(uno, sizeof(uno), "\"%u.%u\"", w.hello->fw >> 8, w.hello->fw & 0xFF);
  return fmt(out, cap,
             "{\"t\":\"welcome\",\"role\":\"human\",\"you\":\"%s\",\"tank\":{\"name\":"
             "\"conqueror-bridge\",\"fw\":{\"esp32\":\"%s\",\"uno\":%s},\"proto\":%u,\"caps\":{"
             "\"encoders\":%s,\"turn\":%s,\"move\":%s,\"drive_rate\":%s,\"calibrate\":%s,\"audio_led\":%s,"
             "\"sensor\":\"%s\"}},\"owner\":\"%s\",\"owner_client\":\"%s\"}",
             w.you, w.fw_esp32, uno, proto::kProtocolVersion, capBit(*w.hello, proto::CapBit::kEncoders),
             localOnly(*w.hello, proto::CapBit::kTurn), moveCap(*w.hello),
             localOnly(*w.hello, proto::CapBit::kDriveRate), capBit(*w.hello, proto::CapBit::kCalibrate),
             capBit(*w.hello, proto::CapBit::kAudioLed), w.sensor, w.owner.owner, w.owner.client);
}

namespace {
const char* primName(PrimKind k) { return k == PrimKind::kRate ? "rate" : k == PrimKind::kMove ? "move" : "turn"; }
}  // namespace

size_t jsonTf(char* out, size_t cap, uint32_t seq, const TankSnapshot& s, PrimitiveView prim) {
  if (!s.fast_valid) return 0;
  const proto::TelemFastMsg& f = s.fast;
  char dist[12] = "null";  // 65535 clear, 65534 sensor fault (+ "dist_fault":true so the page never shows CLEAR)
  if (f.dist_mm < 65534) snprintf(dist, sizeof(dist), "%u", (f.dist_mm + 5u) / 10u);
  const bool cam_moving = bit(f.flags, static_cast<unsigned>(proto::FastFlagBit::kCamMoving));
  char progress[64] = "";
  if (prim.active) {
    snprintf(progress, sizeof(progress), ",\"primitive\":{\"kind\":\"%s\",\"pct\":%u%s}", primName(prim.kind),
             static_cast<unsigned>(prim.pct), prim.limited ? ",\"limited\":true" : "");
  }
  return fmt(out, cap,
             "{\"t\":\"tf\",\"seq\":%lu,\"tank_ms\":%lu,\"yaw\":%.2f,\"yaw_rate\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,"
             "\"dist_cm\":%s%s,\"pan\":%.1f,\"tilt\":%.1f,\"spd_cmd\":[%d,%d],\"cam_moving\":%s%s}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), f.yaw / 100.0, f.yaw_rate / 10.0,
             f.pitch / 100.0, f.roll / 100.0, dist, f.dist_mm == 65534 ? ",\"dist_fault\":true" : "", f.pan / 10.0,
             f.tilt / 10.0, f.spd_cmd_l,
             f.spd_cmd_r, cam_moving ? "true" : "false", progress);
}

size_t jsonTd(char* out, size_t cap, uint32_t seq, const TankSnapshot& s) {
  if (!s.fast_valid) return 0;
  const proto::TelemFastMsg& f = s.fast;
  char act_l[6] = "null", act_r[6] = "null";  // -128 = unknown (encoders off)
  if (f.spd_act_l != -128) snprintf(act_l, sizeof(act_l), "%d", f.spd_act_l);
  if (f.spd_act_r != -128) snprintf(act_r, sizeof(act_r), "%d", f.spd_act_r);
  char dist[8] = "null";  // 65535 no echo, 65534 sensor fault
  if (f.dist_mm < 65534) snprintf(dist, sizeof(dist), "%u", static_cast<unsigned>(f.dist_mm));
  return fmt(out, cap,
             "{\"t\":\"td\",\"seq\":%lu,\"uno_ms\":%lu,\"enc\":[%ld,%ld],\"pwm\":[%d,%d],\"cmd\":[%d,%d],"
             "\"act\":[%s,%s],\"yaw\":%.2f,\"rate\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,\"dist_mm\":%s%s,"
             "\"closing\":%d,\"pan\":%.1f,\"tilt\":%.1f,\"flags\":%u,\"prim\":%u,\"pct\":%u}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), static_cast<long>(f.enc_l),
             static_cast<long>(f.enc_r), f.pwm_l, f.pwm_r, f.spd_cmd_l, f.spd_cmd_r, act_l, act_r, f.yaw / 100.0,
             f.yaw_rate / 10.0, f.pitch / 100.0, f.roll / 100.0, dist, f.dist_mm == 65534 ? ",\"dist_fault\":true" : "",
             f.closing_cm_s, f.pan / 10.0, f.tilt / 10.0, static_cast<unsigned>(f.flags),
             static_cast<unsigned>(f.prim), static_cast<unsigned>(f.prim_pct));
}

size_t jsonTds(char* out, size_t cap, const TankSnapshot& s) {
  if (!s.slow_valid) return 0;
  const proto::TelemSlowMsg& m = s.slow;
  return fmt(out, cap,
             "{\"t\":\"tds\",\"uptime_s\":%lu,\"reset_reason\":%u,\"batt_mv\":%u,\"driving\":%u,\"temp\":%d,"
             "\"owner\":%u,\"flags2\":%lu,\"accel_var\":%u,\"loop_load\":%u,\"loop_max_us\":%u,\"free_ram\":%u,"
             "\"err_frames\":%u,\"err_i2c\":%u,\"settings_crc\":%u,\"env\":[%u,%u,%u],\"profile\":%u,"
             "\"cal\":[%u,%u,%u],\"hw\":%u}",
             static_cast<unsigned long>(m.uptime_s), static_cast<unsigned>(m.reset_reason),
             static_cast<unsigned>(m.batt_mv), static_cast<unsigned>(m.driving), m.temp,
             static_cast<unsigned>(m.owner), static_cast<unsigned long>(m.flags2), static_cast<unsigned>(m.accel_var),
             static_cast<unsigned>(m.loop_load), static_cast<unsigned>(m.loop_max_us),
             static_cast<unsigned>(m.free_ram), static_cast<unsigned>(m.err_frames), static_cast<unsigned>(m.err_i2c),
             static_cast<unsigned>(m.settings_crc), static_cast<unsigned>(m.env_speed_cap),
             static_cast<unsigned>(m.env_accel_cap), static_cast<unsigned>(m.env_lock_brake),
             static_cast<unsigned>(m.profile_idx), static_cast<unsigned>(m.cal_status[0]),
             static_cast<unsigned>(m.cal_status[1]), static_cast<unsigned>(m.cal_status[2]),
             static_cast<unsigned>(m.hw));
}

size_t jsonTs(char* out, size_t cap, const TankSnapshot& s, OwnerView owner, bool body_offline) {
  char flags[200] = "";
  size_t n = 0;
  auto add = [&](bool on, const char* name) {
    if (!on) return;
    const int k = snprintf(flags + n, sizeof(flags) - n, "%s\"%s\"", n ? "," : "", name);
    if (k > 0 && static_cast<size_t>(k) < sizeof(flags) - n) n += static_cast<size_t>(k);
  };
  using FB = proto::FastFlagBit;
  using SB = proto::SlowFlagBit;
  const uint32_t ff = s.fast_valid ? s.fast.flags : 0;
  const uint32_t sf = s.slow_valid ? s.slow.flags2 : 0;
  add(bit(ff, static_cast<unsigned>(FB::kBlocked)), "blocked");
  add(bit(ff, static_cast<unsigned>(FB::kTilted)), "tilted");
  add(bit(ff, static_cast<unsigned>(FB::kLifted)), "lifted");
  add(bit(ff, static_cast<unsigned>(FB::kEncoderFault)), "encoder_fault");  // R6.5: sticky until the Uno restarts
  add(bit(sf, static_cast<unsigned>(SB::kBattLow)), "batt_low");
  add(bit(sf, static_cast<unsigned>(SB::kBattCritical)), "batt_critical");
  add(bit(sf, static_cast<unsigned>(SB::kLinkLost)), "link_lost");
  add(bit(sf, static_cast<unsigned>(SB::kBrakeOff)), "brake_off");
  add(bit(sf, static_cast<unsigned>(SB::kMuted)), "muted");
  add(bit(sf, static_cast<unsigned>(SB::kQuiet)), "quiet");
  add(bit(sf, static_cast<unsigned>(SB::kEnvelope)), "envelope");
  add(bit(sf, static_cast<unsigned>(SB::kSafetyOverride)), "safety_override");
  add(bit(sf, static_cast<unsigned>(SB::kSelftestLocked)), "selftest_locked");
  add(body_offline, "body_offline");
  char batt[12] = "null";
  if (s.slow_valid) snprintf(batt, sizeof(batt), "%.2f", s.slow.batt_mv / 1000.0);
  return fmt(out, cap,
             "{\"t\":\"ts\",\"batt_v\":%s,\"owner\":\"%s\",\"owner_client\":\"%s\",\"flags\":[%s],"
             "\"envelope\":{\"speed_cap\":%u,\"accel_cap\":%u,\"lock_brake\":%u}}",
             batt, owner.owner, owner.client, flags, s.slow_valid ? s.slow.env_speed_cap : 100u,
             s.slow_valid ? s.slow.env_accel_cap : 0u, s.slow_valid ? s.slow.env_lock_brake : 0u);
}

size_t jsonUnoEvent(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint8_t kind, int16_t a,
                    int16_t b) {
  char head[64];
  snprintf(head, sizeof(head), "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":",
           static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms));
  using EK = proto::EventKind;
  switch (static_cast<EK>(kind)) {
    case EK::kIrOverride:
      return fmt(out, cap, "%s\"ir_override\"}", head);
    case EK::kDeadmanStop:
      return fmt(out, cap, "%s\"deadman_stop\"}", head);
    case EK::kBlocked:
      return fmt(out, cap, "%s\"blocked\",\"dist_cm\":%d}", head, (a + 5) / 10);
    case EK::kUnblocked:
      return fmt(out, cap, "%s\"unblocked\"}", head);
    case EK::kTilted:
      return fmt(out, cap, "%s\"tilted\",\"pitch\":%.1f,\"roll\":%.1f}", head, a / 10.0, b / 10.0);
    case EK::kLevel:
      return fmt(out, cap, "%s\"level\"}", head);
    case EK::kLifted:
      return fmt(out, cap, "%s\"lifted\"}", head);
    case EK::kLanded:
      return fmt(out, cap, "%s\"landed\"}", head);
    case EK::kBattLow:
      return fmt(out, cap, "%s\"batt_low\",\"v\":%.2f}", head, static_cast<uint16_t>(a) / 1000.0);
    case EK::kBattCritical:
      return fmt(out, cap, "%s\"batt_critical\",\"v\":%.2f}", head,
                 static_cast<uint16_t>(a) / 1000.0);
    case EK::kLinkLost:
      return fmt(out, cap, "%s\"link_lost\"}", head);
    case EK::kLinkRestored:
      return fmt(out, cap, "%s\"link_restored\"}", head);
    case EK::kSafetyOverride:
      return fmt(out, cap, "%s\"safety_override\",\"on\":%s,\"reason\":\"%s\"}", head,
                 a == 1 ? "true" : "false", overrideReason(b));
    case EK::kStall:  // plan I (spec 02 §F1.2): both tracks stopped until released
      return fmt(out, cap, "%s\"stall\",\"side\":\"%s\"}", head, a == 0 ? "left" : "right");
    case EK::kEncoderFault:
      return fmt(out, cap, "%s\"encoder_fault\",\"side\":\"%s\"}", head, a == 0 ? "left" : "right");
    case EK::kHeadingFault:  // §F1.3
      return fmt(out, cap, "%s\"heading_fault\",\"err_deg\":%d}", head, a);
    default:
      return 0;  // control_changed comes from LinkOwner; the rest is not in the slice
  }
}

size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const PrimitiveEnd& e) {
  static const char* const kReasons[] = {"done", "cancelled", "stop", "not_owner", "stalled", "timeout", "link",
                                         "deadman", "diverged", "blocked", "encoder_fault"};
  const uint8_t r = static_cast<uint8_t>(e.reason);
  char id[20] = "";
  if (e.id >= 0) snprintf(id, sizeof(id), ",\"id\":%ld", static_cast<long>(e.id));
  char moved[24] = "";
  if (e.has_moved) {
    const long mm = static_cast<long>(e.moved_mm);
    const long a = mm < 0 ? -mm : mm;
    snprintf(moved, sizeof(moved), ",\"moved_cm\":%s%ld.%ld", mm < 0 ? "-" : "", a / 10, a % 10);
  }
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"primitive_done\",\"prim\":\"%s\","
             "\"ok\":%s,\"reason\":\"%s\"%s%s}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), primName(e.kind),
             e.ok ? "true" : "false", r < sizeof(kReasons) / sizeof(kReasons[0]) ? kReasons[r] : "done", moved, id);
}

size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm) {
  return fmt(out, cap, "{\"t\":\"calib\",\"counts_per_m\":%u,\"roll_mm\":%u}", static_cast<unsigned>(counts_per_m),
             static_cast<unsigned>(roll_mm));
}

size_t jsonWifiStatus(char* out, size_t cap, const WifiStatusView& v) {
  char ssid[72], saved[3 * 72 + 8] = "";
  jsonEscape(ssid, sizeof(ssid), v.ssid ? v.ssid : "");
  size_t n = 0;
  for (uint8_t i = 0; i < v.n_saved && i < 3; ++i) {
    char e[72];
    jsonEscape(e, sizeof(e), v.saved[i]);
    const int k = snprintf(saved + n, sizeof(saved) - n, "%s\"%s\"", n ? "," : "", e);
    if (k > 0 && static_cast<size_t>(k) < sizeof(saved) - n) n += static_cast<size_t>(k);
  }
  return fmt(out, cap,
             "{\"t\":\"wifi_status\",\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"host\":\"%s\","
             "\"saved\":[%s],\"ap_ssid\":\"%s\"}",
             v.mode, ssid, v.rssi, v.ip, v.host, saved, v.ap_ssid);
}

size_t jsonWifiScan(char* out, size_t cap, const ScanEntry* nets, size_t count) {
  char list[320] = "";
  size_t n = 0;
  for (size_t i = 0; i < count && i < 12; ++i) {
    char e[72];
    jsonEscape(e, sizeof(e), nets[i].ssid);
    const int k = snprintf(list + n, sizeof(list) - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", n ? "," : "",
                           e, nets[i].rssi, nets[i].secure ? "true" : "false");
    if (k <= 0 || static_cast<size_t>(k) >= sizeof(list) - n) break;  // keep what fits (kWsJsonMax)
    n += static_cast<size_t>(k);
  }
  return fmt(out, cap, "{\"t\":\"wifi_scan\",\"nets\":[%s]}", list);
}

size_t jsonConfirm(char* out, size_t cap, const char* what, uint8_t secs) {
  return fmt(out, cap, "{\"t\":\"confirm\",\"what\":\"%s\",\"secs\":%u}", what, static_cast<unsigned>(secs));
}
size_t jsonConfirmed(char* out, size_t cap, const char* what) {
  return fmt(out, cap, "{\"t\":\"confirmed\",\"what\":\"%s\"}", what);
}
size_t jsonConfirmExpired(char* out, size_t cap, const char* what) {
  return fmt(out, cap, "{\"t\":\"confirm_expired\",\"what\":\"%s\"}", what);
}
size_t jsonSetting(char* out, size_t cap, uint8_t id, int32_t value) {
  return fmt(out, cap, "{\"t\":\"setting\",\"id\":%u,\"value\":%ld}", static_cast<unsigned>(id),
             static_cast<long>(value));
}

size_t jsonControlChanged(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, OwnerView owner) {
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"control_changed\",\"owner\":"
             "\"%s\",\"client\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), owner.owner,
             owner.client);
}

size_t jsonBody(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, bool online, int uno_proto) {
  if (online || uno_proto < 0) {
    return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"%s\"}",
               static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms),
               online ? "body_online" : "body_offline");
  }
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"body_offline\",\"reason\":"
             "\"BODY_INCOMPATIBLE\",\"uno_proto\":%d,\"need_proto\":%u}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), uno_proto,
             proto::kProtocolVersion);
}

size_t jsonAck(char* out, size_t cap, int32_t id) {
  if (id < 0) return fmt(out, cap, "{\"t\":\"ack\"}");
  return fmt(out, cap, "{\"t\":\"ack\",\"id\":%ld}", static_cast<long>(id));
}

size_t jsonNackStatus(char* out, size_t cap, int32_t id, uint8_t status) {
  if (status == kStatusTimeout) return jsonNack(out, cap, id, BridgeReason::kTimeout);
  if (status >= kAckReasonCount) return nack(out, cap, id, kAckReasons[17]);  // INTERNAL
  return nack(out, cap, id, kAckReasons[status]);
}

size_t jsonNack(char* out, size_t cap, int32_t id, BridgeReason reason) {
  return nack(out, cap, id, kBridgeReasons[static_cast<size_t>(reason)]);
}

size_t jsonNackMessage(char* out, size_t cap, int32_t id, BridgeReason reason, const char* message) {
  char esc[128];
  if (jsonEscape(esc, sizeof(esc), message) == 0 && message[0] != '\0') return 0;
  ReasonInfo r = kBridgeReasons[static_cast<size_t>(reason)];
  r.message = esc;
  return nack(out, cap, id, r);
}

size_t jsonEscape(char* out, size_t cap, const char* s) {
  if (cap == 0) return 0;
  size_t n = 0;
  for (; *s != '\0'; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    char esc[7];
    size_t k = 0;
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = static_cast<char>(c);
      k = 2;
    } else if (c < 0x20) {
      k = static_cast<size_t>(snprintf(esc, sizeof(esc), "\\u%04x", c));
    } else {
      esc[0] = static_cast<char>(c);
      k = 1;
    }
    if (n + k + 1 > cap) break;
    memcpy(out + n, esc, k);
    n += k;
  }
  out[n] = '\0';
  return n;
}

size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms) {
  return fmt(out, cap, "{\"t\":\"pong\",\"t0\":%s,\"tank_ms\":%lu}", t0,
             static_cast<unsigned long>(tank_ms));
}

uint16_t ackReasonCode(uint8_t status) {
  return status < kAckReasonCount ? kAckReasons[status].code : 0;
}

}  // namespace core
}  // namespace cq
