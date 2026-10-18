// Tank -> WebSocket JSON (bridge LLD R2.9, spec 01 §4 and §6 subset). Writes into caller buffers
// with snprintf; every function returns the length, or 0 when nothing is to be sent / it did not
// fit. Pure C++, no heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/body_link.h"
#include "core/motion_service.h"
#include "core/wifi_store.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr size_t kWsJsonMax = 384;

// Reasons the bridge itself produces (not on the Uno's ack_status list).
enum class BridgeReason : uint8_t { kNotOwner, kBusy, kBodyOffline, kRateLimited, kInvalidCommand, kTimeout, kUnavailable,
                                    kStopped };  // kStopped: design-notes §72 STOP hold

struct OwnerView {
  const char* owner;   // "none" | "human" | "ir"
  const char* client;  // "phone-2" | "bench" | ""
};

struct WelcomeInfo {
  const char* fw_esp32;
  const HelloInfo* hello;  // uno fw / caps (null fields until a HELLO was seen)
  const char* sensor;      // camera sensor name or "none"
  OwnerView owner;
  const char* you;  // this client's name, e.g. "phone-1"
};

size_t jsonWelcome(char* out, size_t cap, const WelcomeInfo& w);
// Bridge motion service progress for tf (Plan H, spec 01 §6.1 `primitive`).
struct PrimitiveView {
  bool active = false;
  PrimKind kind = PrimKind::kNone;
  uint8_t pct = 0;
  bool limited = false;  // Plan L: drive_rate clamped to the slowest smooth spin
};

size_t jsonTf(char* out, size_t cap, uint32_t seq, const TankSnapshot& s, PrimitiveView prim = PrimitiveView{});
// {"t":"ev",...,"kind":"primitive_done","prim":"turn"|"rate"|"move","ok":b,"reason":"done"|...,"moved_cm"? (move),
// "id"?} (bridge LLD R4.5, R7.1)
size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const PrimitiveEnd& e);
size_t jsonTs(char* out, size_t cap, const TankSnapshot& s, OwnerView owner, bool body_offline);
// Plan K (LLD R7.1 / R7.9): {"t":"calib","counts_per_m":n,"roll_mm":m} - reply to calib (read or after a save).
size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm);
// Plan M (spec 02 §I3): WiFi status / scan, the confirmation replies, Uno settings. Never a password.
struct WifiStatusView {
  const char* mode;  // "home" / "connecting" / "ap_fallback" / "ap"
  const char* ssid;
  int rssi;
  const char* ip;
  const char* host;
  const char* saved[3];
  uint8_t n_saved;
  const char* ap_ssid;
};
size_t jsonWifiStatus(char* out, size_t cap, const WifiStatusView& v);
size_t jsonWifiScan(char* out, size_t cap, const ScanEntry* nets, size_t count);
size_t jsonConfirm(char* out, size_t cap, const char* what, uint8_t secs);
size_t jsonConfirmed(char* out, size_t cap, const char* what);
size_t jsonConfirmExpired(char* out, size_t cap, const char* what);
size_t jsonSetting(char* out, size_t cap, uint8_t id, int32_t value);
// LLD R5 debug stream: raw TELEM_FAST ("td") / TELEM_SLOW ("tds") values; 0 before the first frame.
size_t jsonTd(char* out, size_t cap, uint32_t seq, const TankSnapshot& s);
size_t jsonTds(char* out, size_t cap, const TankSnapshot& s);
// EVENT from the Uno; returns 0 for kinds outside the Basic Drive slice.
size_t jsonUnoEvent(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint8_t kind, int16_t a,
                    int16_t b);
size_t jsonControlChanged(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, OwnerView owner);
size_t jsonBody(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, bool online,
                int uno_proto);  // uno_proto >= 0: offline because of an incompatible protocol
size_t jsonAck(char* out, size_t cap, int32_t id);
// id < 0: no "id" (streaming command rejected). status: Uno ack_status index or kStatusTimeout.
size_t jsonNackStatus(char* out, size_t cap, int32_t id, uint8_t status);
size_t jsonNack(char* out, size_t cap, int32_t id, BridgeReason reason);
// Plan L: nack with its own message (JSON-escaped), e.g. the arc band refusal.
size_t jsonNackMessage(char* out, size_t cap, int32_t id, BridgeReason reason, const char* message);
// Escapes s for a JSON string body (" \\ and control characters); never splits an escape at the
// end of out. Returns the length written (out is always NUL-terminated when cap > 0). Review M10.
size_t jsonEscape(char* out, size_t cap, const char* s);
size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms);

// Error code number behind an ack_status index (checked against kAckStatusCode in tests).
uint16_t ackReasonCode(uint8_t status);

// Uno owner -> spec 01 owner name. CALIBRATION shows as "human" (a person runs it from the bench).
const char* ownerName(proto::Owner o);

}  // namespace core
}  // namespace cq
