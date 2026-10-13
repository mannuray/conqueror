#include "domain/motion/primitives.h"

#include "domain/util/math_util.h"

namespace cq {

namespace {
constexpr float kDegToRad = 0.0174533f;
constexpr int16_t kHeadingCurrent = 32767;
}  // namespace

void Primitives::end(bool ok, proto::AckStatus reason) {
  if (m_active == proto::PrimitiveKind::kNone) return;
  m_result = PrimitiveResult{m_active, ok, reason};
  m_has_result = true;
  m_active = proto::PrimitiveKind::kNone;
}

void Primitives::cancel(proto::AckStatus reason) { end(false, reason); }

proto::AckStatus Primitives::startTurn(const proto::TurnMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                                       uint32_t now_ms) {
  if (!cfg.imu_ok) return proto::AckStatus::kUnavailable;
  if (m.speed == 0 || m.speed > 100) return proto::AckStatus::kInvalidCommand;
  if ((m.mode & 0x7F) > static_cast<uint8_t>(proto::TurnMode::kTo)) return proto::AckStatus::kInvalidCommand;
  const bool pivot = (m.mode & 0x80) != 0;
  const proto::TurnMode mode = static_cast<proto::TurnMode>(m.mode & 0x7F);
  float angle = m.angle * 0.1f;
  if (mode == proto::TurnMode::kTo) angle = wrap180(wrap360(angle) - in.yaw_deg);

  float max_rate = m.speed / 100.0f * cfg.turn_rate_max_dps;
  float radius_mm = pivot ? cfg.track_width_mm * 0.5f : m.radius_cm * 10.0f;
  float arc_v_pct = 0.0f;
  if (radius_mm > 0.0f) {
    const float v_allowed = m.speed / 100.0f * cfg.v_full_mm_s;               // forward speed budget
    const float rate_for_v = v_allowed / radius_mm / kDegToRad;               // °/s that budget allows
    if (rate_for_v < max_rate) max_rate = rate_for_v;
    arc_v_pct = max_rate * kDegToRad * radius_mm / cfg.v_full_mm_s * 100.0f;  // keeps v/ω = radius
  }
  // Too slow to profile: the 15 °/s floor would over-speed the arc and the 3 °/s stall check would trip.
  if (max_rate < TurnProfile::kMinRateDps) return proto::AckStatus::kInvalidCommand;
  cancel(proto::AckStatus::kCancelled);
  m_turn.start(TurnParams{angle, max_rate, kTurnAccelDps2, arc_v_pct}, in.yaw_total_deg, now_ms);
  m_active = proto::PrimitiveKind::kTurn;
  return proto::AckStatus::kOk;
}

proto::AckStatus Primitives::startMove(const proto::MoveMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                                       uint32_t now_ms) {
  if (!cfg.encoders || !cfg.imu_ok) return proto::AckStatus::kUnavailable;  // distance + gyro heading hold
  if (m.speed == 0 || m.speed > 100 || m.dist_mm == 0) return proto::AckStatus::kInvalidCommand;
  const float heading = m.heading == kHeadingCurrent ? in.yaw_deg : wrap360(m.heading * 0.1f);
  cancel(proto::AckStatus::kCancelled);
  m_move.start(MoveParams{static_cast<float>(m.dist_mm), static_cast<float>(m.speed), cfg.v_full_mm_s, kMoveAccelMmS2,
                          heading, cfg.counts_per_mm},
               in.enc_l, in.enc_r, now_ms);
  m_active = proto::PrimitiveKind::kMove;
  return proto::AckStatus::kOk;
}

bool Primitives::tick(const PrimitiveInputs& in, float dt_s, uint32_t now_ms, MotionSetpoint& sp) {
  sp = MotionSetpoint{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
  ProfileState st = ProfileState::kRunning;
  switch (m_active) {
    case proto::PrimitiveKind::kTurn:
      st = m_turn.tick(in.yaw_total_deg, in.yaw_rate_dps, dt_s, now_ms, sp);
      break;
    case proto::PrimitiveKind::kMove:
      st = m_move.tick(in.enc_l, in.enc_r, in.stalled, dt_s, now_ms, sp);
      break;
    default:
      return false;
  }
  switch (st) {
    case ProfileState::kRunning: return true;
    case ProfileState::kDone: end(true, proto::AckStatus::kOk); break;
    case ProfileState::kTimeout: end(false, proto::AckStatus::kTimeout); break;
    case ProfileState::kStalled: end(false, proto::AckStatus::kStalled); break;
  }
  sp = MotionSetpoint{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
  return false;
}

bool Primitives::takeResult(PrimitiveResult& out) {
  if (!m_has_result) return false;
  out = m_result;
  m_has_result = false;
  return true;
}

uint8_t Primitives::progressPct() const {
  switch (m_active) {
    case proto::PrimitiveKind::kTurn: return m_turn.progressPct();
    case proto::PrimitiveKind::kMove: return m_move.progressPct();
    default: return 0;
  }
}

}  // namespace cq
