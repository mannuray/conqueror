#include "core/game_mode.h"

namespace cq {
namespace core {

GameMode::Start GameMode::startRun(bool game, bool people, uint8_t cap_pct, uint32_t now_ms) {
  if (m_state == GameState::kHeld) return Start::kHeld;
  if (!game) return Start::kGo;
  if (!people) return m_refusal = Start::kNoPeople;
  if (cap_pct == 0) return m_refusal = Start::kNoCap;
  m_refusal = Start::kGo;
  m_cap = cap_pct;
  m_state = GameState::kArming;
  m_arm_ms = now_ms;
  m_has_person = m_unseen = m_far = m_touch = false;
  m_seen_cm = -1;
  m_env_pending = m_env_on = true;
  return Start::kGo;
}

void GameMode::onEnvelopeAck(uint32_t now_ms) {
  if (m_state != GameState::kArming) return;
  m_state = GameState::kPlaying;
  m_ack_ms = now_ms;
}

void GameMode::onPerson(const PersonEst& e, uint32_t now_ms) {
  if (!active() || ago(now_ms, e.at_ms) > kStaleMs) return;  // too old already: not fresh (a frozen camera)
  m_p = e;
  m_has_person = true;
  m_person_rx = e.at_ms;
  noteSeen(e.seen, now_ms);
  if (e.seen && e.d_cm >= 0) sighting(e.d_cm);
  if (m_state == GameState::kCaught) noteFar(e.seen && e.d_cm >= kReleaseCm, now_ms);
}

// Only a sighting catches (§248): an out-of-view estimate would catch again after a not-found release.
void GameMode::sighting(int16_t d_cm) {
  m_seen_cm = d_cm;
  if (m_state == GameState::kPlaying && d_cm <= KeepOut::kCaughtCm) catchNow();
}

void GameMode::noteSeen(bool seen, uint32_t now_ms) {
  if (seen) {
    m_unseen = false;
  } else if (!m_unseen) {
    m_unseen = true;
    m_unseen_since = now_ms;
  }
}

void GameMode::noteFar(bool far, uint32_t now_ms) {
  if (far && !m_far) m_far_since = now_ms;
  m_far = far;
  releaseCheck(now_ms);
}

void GameMode::onEnvelopeFailed() {
  if (m_state != GameState::kArming) return;
  toOff();
  m_refusal = Start::kNoCap;
}

void GameMode::onCaughtEvent(uint32_t) {
  if (m_state == GameState::kPlaying) catchNow();
  if (m_state == GameState::kCaught) m_touch = true;  // a touch: only a sighting releases it
}

void GameMode::onEngineGone(uint32_t) {
  if (active()) toHeld();
}

void GameMode::onStop(bool press) {
  if (active() && (press || m_state != GameState::kHeld)) toOff();
}

void GameMode::onRunEnd() {
  if (active() && m_state != GameState::kHeld) toOff();
}

bool GameMode::onUnoHello(uint32_t now_ms) {
  if (m_state != GameState::kPlaying && m_state != GameState::kCaught && m_state != GameState::kArming) return false;
  m_state = GameState::kArming;
  m_arm_ms = now_ms;
  m_env_pending = m_env_on = true;
  return true;
}

Gate GameMode::gate(const Move& m, uint16_t stop_cm, uint32_t now_ms) {
  if (m_state == GameState::kOff) return Gate::kAllow;
  if (m_state != GameState::kPlaying && m_state != GameState::kCaught) return Gate::kHoldZero;  // arming, held
  if (!m_has_person || ago(now_ms, m_person_rx) > kStaleMs) return Gate::kHoldZero;
  return m_state == GameState::kCaught ? caughtGate(m) : playGate(m, stop_cm, now_ms);
}

// Caught (§248): the tracks stop; only a turn on the spot keeping the person in view, seen beyond the turn level.
Gate GameMode::caughtGate(const Move& m) const {
  const bool turning = m.v_pct == 0 && m.yaw_rate_dps != 0.0f;
  return turning && m_p.seen && m_p.d_cm > KeepOut::kTurnCm ? Gate::kAllow : Gate::kHoldZero;
}

// Playing: the keep-out; out of view beyond 1 s, a turn on the spot only when last seen from kBlindTurnCm with nothing
// ahead (FACE's look round, §248).
Gate GameMode::playGate(const Move& m, uint16_t stop_cm, uint32_t now_ms) const {
  const uint32_t unseen_ms = m_unseen ? ago(now_ms, m_unseen_since) : 0;
  const bool turning = m.v_pct == 0 && m.yaw_rate_dps != 0.0f;
  if (turning && unseen_ms > KeepOut::kUnseenMs && m_seen_cm >= kBlindTurnCm && aheadClear()) return Gate::kAllow;
  return KeepOut::check(m_p, m, stop_cm, unseen_ms);
}

void GameMode::step(uint32_t now_ms) {
  if (m_state == GameState::kArming && ago(now_ms, m_arm_ms) > kArmAckMs) {
    toOff();
    m_refusal = Start::kNoCap;
    return;
  }
  if ((m_state == GameState::kPlaying || m_state == GameState::kCaught) && ago(now_ms, lastHeard()) > kHeldMs) {
    toHeld();
    return;
  }
  if (m_state == GameState::kCaught) releaseCheck(now_ms);
}

bool GameMode::wantsEnvelope(bool& on) {
  if (!m_env_pending) return false;
  m_env_pending = false;
  on = m_env_on;
  return true;
}

bool GameMode::justCaught() {
  const bool j = m_just_caught;
  m_just_caught = false;
  return j;
}

void GameMode::toOff() {
  m_state = GameState::kOff;
  m_env_pending = true;
  m_env_on = false;
  m_just_caught = false;
}

void GameMode::toHeld() { m_state = GameState::kHeld; }

void GameMode::catchNow() {
  m_state = GameState::kCaught;
  m_just_caught = true;
  m_far = false;
  m_touch = false;
  ++m_caught_n;
}

void GameMode::releaseCheck(uint32_t now_ms) {
  if ((m_far && ago(now_ms, m_far_since) >= kReleaseMs) || notFound(now_ms)) m_state = GameState::kPlaying;
}

// Not found through FACE's sweep (§248): out of view kSweepMs, last seen outside the absolute level, nothing ahead,
// and not a touch (contact, maybe on a side the camera cannot see).
bool GameMode::notFound(uint32_t now_ms) const {
  return !m_touch && m_unseen && ago(now_ms, m_unseen_since) >= kSweepMs && m_seen_cm > KeepOut::kCaughtCm &&
         aheadClear();
}

bool GameMode::aheadClear() const {
  if (m_sonar_mm == 65535) return true;  // no echo
  return m_sonar_mm < 65534 && m_sonar_mm >= static_cast<uint16_t>(kClearCm * 10);
}

}  // namespace core
}  // namespace cq
