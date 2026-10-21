// TURN (spin) and PIVOT on the bridge (bridge LLD R4.3, R18): approach proportional to the angle still to go
// (latency-compensated, minimum command), the last 25° under rate control at the slowest smooth spin, a cut at the
// predicted stop (learned coast), at most one nudge, done once stopped; stall and timeout. Whole numbers, except
// the rate loop.
#pragma once

#include <stdint.h>

#include "core/motion_config.h"
#include "core/rate_loop.h"

namespace cq {
namespace core {

// Relative angle to turn (0.01°, + = CCW / left): `by` as given, or `to` the shortest way from the current yaw.
int32_t turnDelta(uint16_t yaw_cdeg, bool has_to, int32_t by_cdeg, uint16_t to_cdeg);
int32_t wrapDeltaCdeg(int32_t d);  // to -18000..17999
// R18.2: the learned coast, as the roll-on: (3 x old + reading + 2) / 4, kept within kCoastMinMs..kCoastMaxMs.
uint16_t coastAverage(uint16_t old_ms, uint16_t reading_ms);

struct TurnStep {
  int8_t w = 0;  // % for DRIVE (v = 0)
  bool done = false;
  bool timeout = false;
  bool stalled = false;
  uint16_t coast_ms = 0;  // R18: a new coast reading (ms) once the first coast has stopped; 0 = none
};

enum class TurnPhase : uint8_t { kApproach, kSlow, kCoast, kNudge, kNudgeCoast };

class TurnController {
 public:
  // coast_ms: the learned T_coast (predicted stop = pos + rate x T_coast).
  void begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms,
             uint16_t coast_ms = motion::kCoastDefaultMs);
  TurnStep step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  uint8_t progressPct() const;
  TurnPhase phase() const { return m_phase; }

 private:
  int8_t command(int32_t error_cdeg, int16_t rate_ddps) const;  // the approach (proportional)
  void driven(TurnStep& s, int32_t error, int16_t rate_ddps, uint32_t now_ms);
  void coasting(TurnStep& s, int32_t error, uint32_t now_ms);
  bool cutNow(int32_t error_cdeg, int16_t rate_ddps) const;  // the predicted stop reaches the target
  void cut(int16_t rate_ddps, uint32_t now_ms);
  uint16_t coastReading() const;
  bool stopped(uint32_t now_ms);
  bool stalledFor(int8_t w, int16_t rate_ddps, uint32_t now_ms);

  int32_t m_delta = 0;      // target, relative to the start (0.01°)
  int32_t m_pos = 0;        // turned so far, unwrapped (0.01°)
  uint16_t m_last_yaw = 0;
  uint8_t m_speed = 40;
  uint32_t m_start_ms = 0;
  uint32_t m_last_ms = 0;
  uint32_t m_timeout_ms = 0;
  uint16_t m_coast_ms = 60;
  TurnPhase m_phase = TurnPhase::kApproach;
  int8_t m_dir = 1;          // the way it drives now: the turn's sign, or the nudge's (back after an overshoot)
  RateLoop m_rate;
  int32_t m_cut_pos = 0;     // where the first cut happened ...
  int16_t m_cut_rate = 0;    // ... and how fast it was turning then
  uint32_t m_still_since = 0;
  int32_t m_still_pos = 0;   // m_pos when the current stillness hold began
  bool m_stalling = false;
  uint32_t m_stall_since = 0;
};

}  // namespace core
}  // namespace cq
