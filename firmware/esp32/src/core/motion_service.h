// Bridge motion service (spec 02 §E3, bridge LLD R4): runs one TURN or DRIVE_RATE for one WebSocket client on a
// body that cannot (Uno Lite). Driven by each TELEM_FAST (50 Hz); its output is an ordinary DRIVE {v, w}, so
// every Uno safety rule applies. Every end returns w = 0 (the caller then sends DRIVE 0/0 and STOP).
#pragma once

#include <stdint.h>

#include "core/rate_loop.h"
#include "core/turn_controller.h"

namespace cq {
namespace core {

enum class PrimKind : uint8_t { kNone, kTurn, kRate };
enum class EndReason : uint8_t {
  kDone,
  kCancelled,
  kStop,
  kNotOwner,
  kStalled,
  kTimeout,
  kLink,
  kDeadman,
  kDiverged,  // drive_rate saturated while the tank turns the other way (review C1 guard)
};

struct PrimitiveEnd {
  PrimKind kind = PrimKind::kNone;
  bool ok = false;
  EndReason reason = EndReason::kDone;
  int32_t id = -1;
};

struct MotionOut {
  bool send = false;  // send DRIVE {v, w} now
  int8_t v = 0;
  int8_t w = 0;
  bool ended = false;  // the primitive ended with this output (w = 0): then STOP + primitive_done
  PrimitiveEnd end{};
};

class MotionService {
 public:
  void startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms);
  void driveRate(int fd, int8_t v, int16_t rate_ddps, uint32_t now_ms);  // starts or updates the stream
  MotionOut cancel(EndReason why);
  MotionOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  MotionOut tick(uint32_t now_ms);  // telemetry gap / deadman without new frames
  bool active() const { return m_kind != PrimKind::kNone; }
  PrimKind kind() const { return m_kind; }
  int ownerFd() const { return m_fd; }
  uint8_t progressPct() const;

 private:
  MotionOut finish(EndReason why);
  MotionOut stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  MotionOut stepRate(int16_t rate_ddps, uint32_t now_ms);

  PrimKind m_kind = PrimKind::kNone;
  int m_fd = -1;
  int32_t m_id = -1;
  TurnController m_turn;
  RateLoop m_rate;
  int8_t m_rate_v = 0;
  int16_t m_rate_target = 0;
  uint32_t m_rate_update_ms = 0;
  bool m_wrong_way = false;  // C1 guard: saturated with the measured rate opposite to the target ...
  uint32_t m_wrong_since = 0;  // ... since
  bool m_rate_stalling = false;  // final review I1: saturated while the tank does not turn ...
  uint32_t m_rate_stall_since = 0;  // ... since
  uint32_t m_last_telem_ms = 0;
};

}  // namespace core
}  // namespace cq
