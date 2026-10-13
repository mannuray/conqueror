#include "domain/safety/safety_override.h"

namespace cq {

namespace {
bool ownerMatches(SafetyOverride::Binder b, proto::Owner o) {
  return (b == SafetyOverride::Binder::kIr && o == proto::Owner::kIr) ||
         (b == SafetyOverride::Binder::kNetwork && o == proto::Owner::kNetwork);
}
}  // namespace

void SafetyOverride::post(bool on, proto::OverrideReason why) {
  m_ev_pending = true;
  m_ev_on = on;
  m_ev_why = why;
}

proto::AckStatus SafetyOverride::enter(Binder b, proto::OverrideReason why, uint32_t now_ms) {
  if (m_binder == b) return proto::AckStatus::kOk;  // already on for this side: no event
  m_binder = b;
  m_since_ms = now_ms;
  m_last_drive_ms = now_ms;
  post(true, why);
  return proto::AckStatus::kOk;
}

void SafetyOverride::end(proto::OverrideReason why) {
  if (!active()) return;
  m_binder = Binder::kNone;
  post(false, why);
}

proto::AckStatus SafetyOverride::requestNetwork(bool on, proto::Source src, const Gate& g, uint32_t now_ms) {
  if (!on) {
    end(proto::OverrideReason::kNetwork);
    return proto::AckStatus::kOk;
  }
  if (src != proto::Source::kHuman) return proto::AckStatus::kRoleForbidden;  // never the AI
  if (!g.synced) return proto::AckStatus::kBusy;
  if (g.child_play) return proto::AckStatus::kEnvelopeLocked;  // child play keeps all safety
  if (g.owner == proto::Owner::kIr || g.owner == proto::Owner::kCalibration) return proto::AckStatus::kNotOwner;
  if (m_binder == Binder::kIr) return proto::AckStatus::kNotOwner;
  if (g.fault_latched) return proto::AckStatus::kBlocked;
  return enter(Binder::kNetwork, proto::OverrideReason::kNetwork, now_ms);
}

proto::AckStatus SafetyOverride::requestIr(const Gate& g, uint32_t now_ms) {
  if (m_binder == Binder::kIr) {
    end(proto::OverrideReason::kIr);
    return proto::AckStatus::kOk;
  }
  if (g.child_play) return proto::AckStatus::kEnvelopeLocked;
  if (g.fault_latched) return proto::AckStatus::kBlocked;
  if (active()) return proto::AckStatus::kNotOwner;  // network-bound: the IR takeover (onOwner) must end it first
  return enter(Binder::kIr, proto::OverrideReason::kIr, now_ms);
}

void SafetyOverride::onOwner(proto::Owner owner) {
  if (active() && owner != proto::Owner::kNone && !ownerMatches(m_binder, owner)) {
    end(proto::OverrideReason::kOwnerChange);
  }
}

void SafetyOverride::onNetworkMotion(proto::Source src, bool driving, uint32_t now_ms) {
  if (!active()) return;
  if (src != proto::Source::kHuman || m_binder != Binder::kNetwork) {
    end(proto::OverrideReason::kOwnerChange);  // AI motion, or network motion while IR-bound
    return;
  }
  if (driving) m_last_drive_ms = now_ms;
}

void SafetyOverride::onIrMotion(bool driving, uint32_t now_ms) {
  if (m_binder == Binder::kIr && driving) m_last_drive_ms = now_ms;
}

void SafetyOverride::onOwnerHint(proto::OwnerHintValue hint) {
  if (hint == proto::OwnerHintValue::kAi && m_binder == Binder::kNetwork) end(proto::OverrideReason::kOwnerChange);
}

void SafetyOverride::onLinkLost() {
  if (m_binder == Binder::kNetwork) end(proto::OverrideReason::kLinkLost);
}

void SafetyOverride::tick(uint32_t now_ms, const Config& cfg) {
  if (!active()) return;
  if (now_ms - m_since_ms >= static_cast<uint32_t>(cfg.max_s) * 1000u) {
    end(proto::OverrideReason::kMaxTime);
  } else if (now_ms - m_last_drive_ms >= static_cast<uint32_t>(cfg.idle_s) * 1000u) {
    end(proto::OverrideReason::kIdle);
  }
}

bool SafetyOverride::takeEvent(bool& on, proto::OverrideReason& why) {
  if (!m_ev_pending) return false;
  m_ev_pending = false;
  on = m_ev_on;
  why = m_ev_why;
  return true;
}

}  // namespace cq
