// Bridge motion service (spec 02 §E3, bridge LLD R4 / R7 / R8): runs one TURN, DRIVE_RATE, MOVE (Plan K), arc or
// pivot (Plan L) for one WebSocket client on a body that cannot (Uno Lite). Driven by each TELEM_FAST (50 Hz); its
// output is an ordinary DRIVE {v, w}, so every Uno safety rule applies. Every end returns w = 0 (the caller then sends DRIVE 0/0 and STOP).
#pragma once

#include <stdint.h>

#include "core/arc_controller.h"
#include "core/motion_config.h"
#include "core/move_controller.h"
#include "core/rate_loop.h"
#include "core/turn_controller.h"

namespace cq {
namespace core {

enum class PrimKind : uint8_t { kNone, kTurn, kRate, kMove, kArc, kPivot, kGo };  // kGo: bridge LLD R20
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
  kBlocked,       // Plan K: the obstacle brake held a move for 2 s
  kEncoderFault,  // Plan K: the Uno reported an encoder fault during a move (distance not trusted)
};

struct PrimitiveEnd {
  PrimKind kind = PrimKind::kNone;
  bool ok = false;
  EndReason reason = EndReason::kDone;
  int32_t id = -1;
  bool has_moved = false;  // Plan K: a move reports its counted distance
  int32_t moved_mm = 0;
  uint16_t coast_ms = 0;  // R18: a turn / pivot's coast reading (ms), 0 = none
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
  // coast_ms: the learned T_coast (R18).
  void startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms,
                 uint16_t coast_ms = motion::kCoastDefaultMs);
  void driveRate(int fd, int8_t v, int16_t rate_ddps, uint32_t now_ms);  // starts or updates the stream
  MotionOut cancel(EndReason why);
  // R20: GO - drive straight on at speed_pct (w = 0, the Uno's heading hold) until held, replaced or blocked; the
  // owner refreshes it (keepGo) at least every kRateDeadmanMs. startGo while going: a new speed, refreshed.
  void startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms);
  void keepGo(uint32_t now_ms) {
    if (m_kind == PrimKind::kGo) m_go_update_ms = now_ms;
  }
  MotionOut hold();  // ends a GO as done (v = 0); nothing going: nothing
  void startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, int32_t id,
                 uint32_t now_ms, uint16_t roll_mm = 0);
  void startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm,
                int32_t id, uint32_t now_ms);
  void startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms,
                  uint16_t coast_ms = motion::kCoastDefaultMs);
  MotionOut onTelemetry(const MotionTelem& t, uint32_t now_ms);
  MotionOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {  // turn / rate callers
    MotionTelem t;
    t.yaw_cdeg = yaw_cdeg;
    t.rate_ddps = rate_ddps;
    return onTelemetry(t, now_ms);
  }
  MotionOut tick(uint32_t now_ms);  // telemetry gap / deadman without new frames
  bool active() const { return m_kind != PrimKind::kNone; }
  PrimKind kind() const { return m_kind; }
  int ownerFd() const { return m_fd; }
  uint8_t progressPct() const;
  bool limited() const { return m_kind == PrimKind::kRate && m_rate_limited; }  // Plan L: spin clamped to 65 °/s
  int16_t rateTargetDdps() const { return m_rate_target; }

 private:
  MotionOut finish(EndReason why);
  MotionOut stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  MotionOut stepGo(const MotionTelem& t, uint32_t now_ms);
  MotionOut stepRate(int16_t rate_ddps, uint32_t now_ms);
  bool rateDiverged(bool saturated, int16_t rate_ddps, uint32_t now_ms);
  bool rateStalled(bool saturated, int16_t rate_ddps, uint32_t now_ms);
  MotionOut stepMove(const MotionTelem& t, uint32_t now_ms);
  MotionOut stepArc(const MotionTelem& t, uint32_t now_ms);
  MotionOut stepPivot(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);

  PrimKind m_kind = PrimKind::kNone;
  int m_fd = -1;
  int32_t m_id = -1;
  TurnController m_turn;
  RateLoop m_rate;
  MoveController m_move;
  ArcController m_arc;
  int8_t m_pivot_dir = 1;  // +1 left pivot (left track still), -1 right pivot
  uint16_t m_coast_reading = 0;  // R18: kept until the turn / pivot ends
  int8_t m_rate_v = 0;
  int8_t m_go_v = 0;            // R20
  uint32_t m_go_update_ms = 0;  // R20: the last startGo / keepGo
  bool m_go_ref_set = false;    // R20 (final review C1): progress reference - counts and the distance ahead
  int32_t m_go_ref_counts = 0;
  uint16_t m_go_ref_dist = 0;
  uint32_t m_go_ref_ms = 0;
  bool goStopped(const MotionTelem& t, uint32_t now_ms);  // no progress for kMoveStallMs
  int16_t m_rate_target = 0;
  bool m_rate_limited = false;
  uint32_t m_rate_update_ms = 0;
  bool m_wrong_way = false;  // C1 guard: saturated with the measured rate opposite to the target ...
  uint32_t m_wrong_since = 0;  // ... since
  bool m_rate_stalling = false;  // final review I1: saturated while the tank does not turn ...
  uint32_t m_rate_stall_since = 0;  // ... since
  uint32_t m_last_telem_ms = 0;
};

}  // namespace core
}  // namespace cq
