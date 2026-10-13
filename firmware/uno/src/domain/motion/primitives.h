#pragma once
#include <stdint.h>

#include "domain/motion/motion_setpoint.h"
#include "domain/motion/move_profile.h"
#include "domain/motion/turn_profile.h"
#include "generated/cq_protocol.h"

namespace cq {

struct PrimitiveConfig {
  uint16_t turn_rate_max_dps;
  uint16_t track_width_mm;
  float v_full_mm_s;
  float counts_per_mm;
  bool encoders;
  bool imu_ok;
};

struct PrimitiveInputs {
  float yaw_deg;
  float yaw_total_deg;
  float yaw_rate_dps;
  int32_t enc_l;
  int32_t enc_r;
  bool stalled;
};

struct PrimitiveResult {
  proto::PrimitiveKind kind;
  bool ok;
  proto::AckStatus reason;
};

// One primitive at a time. A cancelled primitive never resumes (link loss, STOP, CANCEL, IR, new command).
class Primitives {
 public:
  static constexpr float kTurnAccelDps2 = 360.0f;
  static constexpr float kMoveAccelMmS2 = 400.0f;

  proto::AckStatus startTurn(const proto::TurnMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                             uint32_t now_ms);
  proto::AckStatus startMove(const proto::MoveMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                             uint32_t now_ms);
  void cancel(proto::AckStatus reason);
  bool tick(const PrimitiveInputs& in, float dt_s, uint32_t now_ms, MotionSetpoint& sp);
  bool takeResult(PrimitiveResult& out);
  proto::PrimitiveKind active() const { return m_active; }
  uint8_t progressPct() const;

 private:
  void end(bool ok, proto::AckStatus reason);
  proto::PrimitiveKind m_active = proto::PrimitiveKind::kNone;
  TurnProfile m_turn;
  MoveProfile m_move;
  PrimitiveResult m_result{proto::PrimitiveKind::kNone, false, proto::AckStatus::kOk};
  bool m_has_result = false;
};

}  // namespace cq
