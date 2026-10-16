#include "core/ws_json.h"

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
// move still needs the body's encoders.
const char* localOnly(const HelloInfo& h, proto::CapBit b) {
  return (h.seen && !(h.caps & (1u << static_cast<uint8_t>(b)))) ? "true" : "false";
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
             localOnly(*w.hello, proto::CapBit::kTurn), capBit(*w.hello, proto::CapBit::kMove),
             localOnly(*w.hello, proto::CapBit::kDriveRate), capBit(*w.hello, proto::CapBit::kCalibrate),
             capBit(*w.hello, proto::CapBit::kAudioLed), w.sensor, w.owner.owner, w.owner.client);
}

namespace {
const char* primName(PrimKind k) { return k == PrimKind::kRate ? "rate" : "turn"; }
}  // namespace

size_t jsonTf(char* out, size_t cap, uint32_t seq, const TankSnapshot& s, PrimitiveView prim) {
  if (!s.fast_valid) return 0;
  const proto::TelemFastMsg& f = s.fast;
  char dist[12] = "null";  // 65535 clear, 65534 sensor fault (+ "dist_fault":true so the page never shows CLEAR)
  if (f.dist_mm < 65534) snprintf(dist, sizeof(dist), "%u", (f.dist_mm + 5u) / 10u);
  const bool cam_moving = bit(f.flags, static_cast<unsigned>(proto::FastFlagBit::kCamMoving));
  char progress[48] = "";
  if (prim.active) {
    snprintf(progress, sizeof(progress), ",\"primitive\":{\"kind\":\"%s\",\"pct\":%u}", primName(prim.kind),
             static_cast<unsigned>(prim.pct));
  }
  return fmt(out, cap,
             "{\"t\":\"tf\",\"seq\":%lu,\"tank_ms\":%lu,\"yaw\":%.2f,\"yaw_rate\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,"
             "\"dist_cm\":%s%s,\"pan\":%.1f,\"tilt\":%.1f,\"spd_cmd\":[%d,%d],\"cam_moving\":%s%s}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), f.yaw / 100.0, f.yaw_rate / 10.0,
             f.pitch / 100.0, f.roll / 100.0, dist, f.dist_mm == 65534 ? ",\"dist_fault\":true" : "", f.pan / 10.0,
             f.tilt / 10.0, f.spd_cmd_l,
             f.spd_cmd_r, cam_moving ? "true" : "false", progress);
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
    default:
      return 0;  // control_changed comes from LinkOwner; the rest is not in the slice
  }
}

size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const PrimitiveEnd& e) {
  static const char* const kReasons[] = {"done", "cancelled", "stop", "not_owner", "stalled", "timeout", "link",
                                         "deadman", "diverged"};
  const uint8_t r = static_cast<uint8_t>(e.reason);
  char id[20] = "";
  if (e.id >= 0) snprintf(id, sizeof(id), ",\"id\":%ld", static_cast<long>(e.id));
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"primitive_done\",\"prim\":\"%s\","
             "\"ok\":%s,\"reason\":\"%s\"%s}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), primName(e.kind),
             e.ok ? "true" : "false", r < sizeof(kReasons) / sizeof(kReasons[0]) ? kReasons[r] : "done", id);
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
