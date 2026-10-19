#include "core/ws_json.h"
#include "core/ws_json_util.h"
#include "core/motion_router.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {
using namespace detail;

namespace {

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

namespace {

struct FlagName {
  bool slow;  // false: TELEM_FAST flags, true: TELEM_SLOW flags2
  unsigned bit;
  const char* name;
};
constexpr FlagName kTsFlags[] = {
    {false, static_cast<unsigned>(proto::FastFlagBit::kBlocked), "blocked"},
    {false, static_cast<unsigned>(proto::FastFlagBit::kTilted), "tilted"},
    {false, static_cast<unsigned>(proto::FastFlagBit::kLifted), "lifted"},
    {false, static_cast<unsigned>(proto::FastFlagBit::kEncoderFault), "encoder_fault"},  // R6.5: sticky until restart
    {true, static_cast<unsigned>(proto::SlowFlagBit::kBattLow), "batt_low"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kBattCritical), "batt_critical"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kLinkLost), "link_lost"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kBrakeOff), "brake_off"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kMuted), "muted"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kQuiet), "quiet"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kEnvelope), "envelope"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kSafetyOverride), "safety_override"},
    {true, static_cast<unsigned>(proto::SlowFlagBit::kSelftestLocked), "selftest_locked"},
};

// Appends "name" (comma-separated) at out + n when it fits; returns the new length.
size_t appendName(char* out, size_t cap, size_t n, const char* name) {
  const int k = snprintf(out + n, cap - n, "%s\"%s\"", n ? "," : "", name);
  return (k > 0 && static_cast<size_t>(k) < cap - n) ? n + static_cast<size_t>(k) : n;
}

// The ts "flags" list: "a","b",... (no brackets).
void tsFlags(char* out, size_t cap, const TankSnapshot& s, bool body_offline) {
  const uint32_t ff = s.fast_valid ? s.fast.flags : 0;
  const uint32_t sf = s.slow_valid ? s.slow.flags2 : 0;
  size_t n = 0;
  out[0] = '\0';
  for (const FlagName& f : kTsFlags) {
    if (bit(f.slow ? sf : ff, f.bit)) n = appendName(out, cap, n, f.name);
  }
  if (body_offline) appendName(out, cap, n, "body_offline");
}

}  // namespace

size_t jsonTs(char* out, size_t cap, const TankSnapshot& s, OwnerView owner, bool body_offline) {
  char flags[200];
  tsFlags(flags, sizeof(flags), s, body_offline);
  char batt[12] = "null";
  unsigned speed_cap = 100, accel_cap = 0, lock_brake = 0;  // no TELEM_SLOW yet: no envelope
  if (s.slow_valid) {
    snprintf(batt, sizeof(batt), "%.2f", s.slow.batt_mv / 1000.0);
    speed_cap = s.slow.env_speed_cap;
    accel_cap = s.slow.env_accel_cap;
    lock_brake = s.slow.env_lock_brake;
  }
  return fmt(out, cap,
             "{\"t\":\"ts\",\"batt_v\":%s,\"owner\":\"%s\",\"owner_client\":\"%s\",\"flags\":[%s],"
             "\"envelope\":{\"speed_cap\":%u,\"accel_cap\":%u,\"lock_brake\":%u}}",
             batt, owner.owner, owner.client, flags, speed_cap, accel_cap, lock_brake);
}

namespace {

bool plainEvent(proto::EventKind k) {  // sent with no values
  using EK = proto::EventKind;
  return k == EK::kIrOverride || k == EK::kDeadmanStop || k == EK::kUnblocked || k == EK::kLevel ||
         k == EK::kLifted || k == EK::kLanded || k == EK::kLinkLost || k == EK::kLinkRestored;
}

const char* side(int16_t a) { return a == 0 ? "left" : "right"; }

// The values after "kind" for the events that carry some; false: not sent to the pages.
bool eventFields(char* out, size_t cap, proto::EventKind k, int16_t a, int16_t b) {
  using EK = proto::EventKind;
  switch (k) {
    case EK::kBlocked: return fmt(out, cap, ",\"dist_cm\":%d", (a + 5) / 10) > 0;
    case EK::kTilted: return fmt(out, cap, ",\"pitch\":%.1f,\"roll\":%.1f", a / 10.0, b / 10.0) > 0;
    case EK::kBattLow:
    case EK::kBattCritical: return fmt(out, cap, ",\"v\":%.2f", static_cast<uint16_t>(a) / 1000.0) > 0;
    case EK::kSafetyOverride: return fmt(out, cap, ",\"on\":%s,\"reason\":\"%s\"", tf(a == 1), overrideReason(b)) > 0;
    case EK::kStall:  // plan I (spec 02 §F1.2): both tracks stopped until released
    case EK::kEncoderFault: return fmt(out, cap, ",\"side\":\"%s\"", side(a)) > 0;
    case EK::kHeadingFault: return fmt(out, cap, ",\"err_deg\":%d", a) > 0;  // §F1.3
    default: return false;  // control_changed comes from LinkOwner; the rest is not in the slice
  }
}

}  // namespace

size_t jsonUnoEvent(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint8_t kind, int16_t a,
                    int16_t b) {
  const auto k = static_cast<proto::EventKind>(kind);
  char fields[64] = "";
  if (!plainEvent(k) && !eventFields(fields, sizeof(fields), k, a, b)) return 0;
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"%s\"%s}", static_cast<unsigned long>(seq),
             static_cast<unsigned long>(tank_ms), unoEventName(kind), fields);
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

}  // namespace core
}  // namespace cq
