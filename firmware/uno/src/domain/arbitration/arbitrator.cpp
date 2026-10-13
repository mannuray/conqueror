#include "domain/arbitration/arbitrator.h"

#include "domain/util/math_util.h"

namespace cq {

proto::AckStatus Arbitrator::requestNetwork(bool teleop, uint32_t now_ms) {
  if (m_owner == proto::Owner::kIr) return proto::AckStatus::kNotOwner;
  if (m_owner == proto::Owner::kCalibration || !m_synced) return proto::AckStatus::kBusy;
  setOwner(proto::Owner::kNetwork);
  m_teleop = teleop;
  m_last_network_ms = now_ms;
  if (teleop) m_last_teleop_ms = now_ms;
  return proto::AckStatus::kOk;
}

void Arbitrator::onNetworkStop() {
  m_teleop = false;
  if (m_owner == proto::Owner::kNetwork) setOwner(proto::Owner::kNone);
}

bool Arbitrator::onIrInput(uint32_t now_ms) {
  m_last_ir_ms = now_ms;
  if (m_owner == proto::Owner::kIr) return false;
  m_teleop = false;
  setOwner(proto::Owner::kIr);
  return true;
}

proto::AckStatus Arbitrator::beginCalibration() {
  if (m_owner == proto::Owner::kIr) return proto::AckStatus::kNotOwner;
  if (!m_synced) return proto::AckStatus::kBusy;  // calibration is started from the network
  m_teleop = false;
  setOwner(proto::Owner::kCalibration);
  return proto::AckStatus::kOk;
}

void Arbitrator::endCalibration() {
  if (m_owner == proto::Owner::kCalibration) setOwner(proto::Owner::kNone);
}

Arbitrator::Action Arbitrator::tick(uint32_t now_ms, bool link_alive, bool primitive_busy, const Config& cfg) {
  switch (m_owner) {
    case proto::Owner::kIr:
      if (elapsed(now_ms, m_last_ir_ms, cfg.ir_handback_ms)) {
        setOwner(proto::Owner::kNone);
        return Action::kIrReleased;
      }
      return Action::kNone;
    case proto::Owner::kNetwork:
    case proto::Owner::kCalibration:
      if (!link_alive) {
        m_teleop = false;
        setOwner(proto::Owner::kNone);
        return Action::kLinkLostStop;
      }
      if (m_teleop && elapsed(now_ms, m_last_teleop_ms, cfg.deadman_ms)) {
        m_teleop = false;
        return Action::kDeadmanStop;
      }
      if (m_owner == proto::Owner::kNetwork && !m_teleop && !primitive_busy &&
          elapsed(now_ms, m_last_network_ms, kNetworkIdleMs)) {
        setOwner(proto::Owner::kNone);  // bookkeeping only (CONTROL_CHANGED); nothing is moving
      }
      return Action::kNone;
    case proto::Owner::kNone:
      return Action::kNone;
  }
  return Action::kNone;
}

}  // namespace cq
