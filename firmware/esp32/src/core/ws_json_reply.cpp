#include "core/ws_json.h"
#include "core/ws_json_util.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {
using namespace detail;

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
    {3011, "STOPPED", "Stopped by another device - release the stick to drive again"},  // design-notes §72
    {3016, "CHECK_AGAIN", "The program changed - check it again"},                       // Plan N3 (bridge only)
    {3017, "NOT_RUNNING", "No program is running"},
};
static_assert(sizeof(kBridgeReasons) / sizeof(kBridgeReasons[0]) == static_cast<size_t>(BridgeReason::kNotRunning) + 1,
              "one row per BridgeReason");

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

size_t nack(char* out, size_t cap, int32_t id, const ReasonInfo& r) {
  char id_part[24] = "";
  if (id >= 0) snprintf(id_part, sizeof(id_part), "\"id\":%ld,", static_cast<long>(id));
  return fmt(out, cap, "{\"t\":\"nack\",%s\"code\":\"%s_%u\",\"reason\":\"%s\",\"message\":\"%s\"}",
             id_part, domain(r.code), r.code, r.reason, r.message);
}

}  // namespace

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

size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms) {
  return fmt(out, cap, "{\"t\":\"pong\",\"t0\":%s,\"tank_ms\":%lu}", t0,
             static_cast<unsigned long>(tank_ms));
}

uint16_t ackReasonCode(uint8_t status) {
  return status < kAckReasonCount ? kAckReasons[status].code : 0;
}

}  // namespace core
}  // namespace cq
