#include "core/keep_out.h"

namespace cq {
namespace core {

namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }

// The bearing folded to −180..180.
int16_t wrap(int16_t deg) {
  int32_t d = deg % 360;
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  return static_cast<int16_t>(d);
}

bool known(const PersonEst& e) { return e.d_cm >= 0; }

Gate forward(const PersonEst& e, uint16_t stop_cm) {
  if (!known(e)) return Gate::kHoldZero;  // no distance: nothing forward
  if (absI(wrap(e.bearing_deg)) > KeepOut::kAheadDeg) return Gate::kAllow;  // aside or behind: not towards
  const int32_t left = static_cast<int32_t>(e.d_cm) - stop_cm - KeepOut::kMarginCm;
  return left > KeepOut::kCaughtCm ? Gate::kAllow : Gate::kEndKeptAway;
}
}  // namespace

Gate KeepOut::check(const PersonEst& e, const Move& m, uint16_t stop_cm, uint32_t unseen_ms) {
  const bool turning = m.v_pct == 0 && m.yaw_rate_dps != 0.0f;
  if (m.v_pct == 0 && !turning) return Gate::kAllow;  // standing still is always safe
  if (m.v_pct < 0) return Gate::kAllow;  // backing away: checked before it starts (back_ok), caught still applies
  if (unseen_ms > kUnseenMs) return Gate::kHoldZero;  // out of sight: hold, then look (FACE's look-round is G2)
  if (turning) return known(e) && e.d_cm > kTurnCm ? Gate::kAllow : Gate::kHoldZero;
  return forward(e, stop_cm);
}

}  // namespace core
}  // namespace cq
