// Tank -> WebSocket JSON (bridge LLD R2.9, spec 01 §4 and §6 subset). Writes into caller buffers
// with snprintf; every function returns the length, or 0 when nothing is to be sent / it did not
// fit. Pure C++, no heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/body_link.h"
#include "core/motion_service.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr size_t kWsJsonMax = 384;

// Reasons the bridge itself produces (not on the Uno's ack_status list).
enum class BridgeReason : uint8_t { kNotOwner, kBusy, kBodyOffline, kRateLimited, kInvalidCommand, kTimeout };

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
};

size_t jsonTf(char* out, size_t cap, uint32_t seq, const TankSnapshot& s, PrimitiveView prim = PrimitiveView{});
// {"t":"ev",...,"kind":"primitive_done","prim":"turn"|"rate","ok":b,"reason":"done"|...,"id"?} (bridge LLD R4.5)
size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const PrimitiveEnd& e);
size_t jsonTs(char* out, size_t cap, const TankSnapshot& s, OwnerView owner, bool body_offline);
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
