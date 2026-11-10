#include "app/game_controller.h"

namespace cq {
namespace app {

namespace {
int8_t absI8(int8_t v) { return v < 0 ? static_cast<int8_t>(-v) : v; }
}  // namespace

uint16_t GameController::onRunStart(bool game, uint32_t now) {
  const bool people = m_h.people != nullptr && m_h.people();
  switch (m_mode.startRun(game, people, m_cap, now)) {
    case core::GameMode::Start::kHeld: return 7052;  // review I2: held until a STOP
    case core::GameMode::Start::kNoPeople: return 7061;
    case core::GameMode::Start::kNoCap: return 7060;
    default: break;
  }
  m_back = Back::kNone;
  m_back_cm = 0;
  m_holding = false;
  return 0;
}

void GameController::onRunEnd(uint32_t now) {
  m_mode.onRunEnd();
  tick(now);
}

void GameController::onStop(uint32_t now) {
  m_mode.onStop();
  tick(now);
}

void GameController::onBodyOnline(uint32_t now) {
  if (m_mode.onUnoHello(now)) return;  // a game: armed again in a fresh window (GameMode wants the envelope)
  if (m_mode.state() == core::GameState::kOff) {  // boot, or an Uno restart with no game: never a stale cap
    m_env_due = true;
    m_env_on = false;
  }
}

void GameController::onPerson(const core::PersonEst& e, uint32_t now) {
  m_now = now;
  m_mode.onPerson(e, now);
}

// Review C2: our ENVELOPEs are answered in the order they were queued; only an ok answer to the last one queued, when
// it is the game's, arms. A refused game envelope while arming ends the run (7060); a refused clear is sent again.
void GameController::onEnvelopeDone(uint8_t status, bool on, uint32_t now) {
  if (m_env_queued > 0) --m_env_queued;
  const bool last = m_env_queued == 0;
  if (status != 0) {
    if (on) m_mode.onEnvelopeFailed();
    if (!on && last && !m_env_last_on) m_env_due = true;  // the Uno must not stay capped
    return;
  }
  if (on && last && m_env_last_on) m_mode.onEnvelopeAck(now);
}

void GameController::onAnswer(int32_t id, int32_t value, uint32_t now) {
  if (m_back != Back::kAsking || id != m_back_id || now - m_back_at > kBackAnswerMs) return;
  m_back = value != 0 ? Back::kYes : Back::kNo;
  m_back_at = now;
}

void GameController::tick(uint32_t now) {
  m_now = now;
  if (m_mode.state() == core::GameState::kArming) m_was_arming = true;
  m_mode.step(now);
  lapse(now);
  bool on = false;
  if (m_mode.wantsEnvelope(on)) {
    m_env_due = true;
    m_env_on = on;
  }
  envelope();
  if (m_mode.justCaught() && m_h.caught != nullptr) m_h.caught(now);
  if (m_keep_away) {
    m_keep_away = false;
    if (m_h.keep_away != nullptr) m_h.keep_away(now);
  }
  transitions(now);
}

// What runs out with time: an unanswered back_ok (the move ends now, not at its next DRIVE), a yes after one move's
// 3 s, and a hold under a running move (review I4: the motion's own watches must never see a long hold).
void GameController::lapse(uint32_t now) {
  backTimeout(now);
  if (m_back == Back::kYes && now - m_back_at > kBackYesMs) m_back = Back::kNone;
  if (m_holding && now - m_hold_since > kHoldEndMs) {
    m_holding = false;
    m_keep_away = true;
  }
}

// The envelope wanted (the game's, or cleared - also the boot clear), retried while the queue is full.
void GameController::envelope() {
  if (!m_env_due || m_h.queue_envelope == nullptr) return;
  if (!m_h.queue_envelope(m_env_on ? m_cap : 100, 0, m_env_on ? kChildPlay : 0)) return;
  m_env_due = false;
  ++m_env_queued;
  m_env_last_on = m_env_on;
}

// A state changed: the pages hear it; Held and a failed arming end the run; the engine's stream follows the game.
void GameController::transitions(uint32_t now) {
  const core::GameState s = m_mode.state();
  engine(s != core::GameState::kOff);
  if (s == m_last) return;
  m_last = s;
  if (m_h.event != nullptr) m_h.event(s, m_mode.lastDistance(), m_mode.caughtCount());
  if (m_h.end_run == nullptr) return;
  if (s == core::GameState::kHeld) m_h.end_run(7052, now);
  if (s == core::GameState::kOff && m_was_arming && m_mode.lastRefusal() == core::GameMode::Start::kNoCap) {
    m_h.end_run(7060, now);
  }
  if (s != core::GameState::kArming) m_was_arming = false;
}

void GameController::engine(bool on) {
  if (on == m_game_on) return;
  m_game_on = on;
  if (m_h.send_game != nullptr) m_h.send_game(on);
}

bool GameController::gateClamp(void* ctx, int8_t& v, int8_t& w, proto::Source src) {
  return ctx == nullptr || static_cast<GameController*>(ctx)->allows(v, w, src);
}

bool GameController::allows(int8_t& v, int8_t& w, proto::Source src) {
  if (m_mode.state() == core::GameState::kOff) return true;
  clamp(v, w);  // review I7: the game's speed cap on the bridge's own commands too
  if (v >= 0) m_back = Back::kNone;  // a backward move ended: the next one asks again (a yes also lapses at 3 s)
  const bool moving = v != 0 || w != 0;
  if (v < 0 && m_mode.state() == core::GameState::kPlaying && !backAllowed(src, m_now)) {
    hold(true, moving);
    return false;
  }
  const core::Move m{v, static_cast<float>(w), v == 0 && w != 0};
  const core::Gate g = m_mode.gate(m, stopCm(v), m_now);
  if (g == core::Gate::kEndKeptAway) m_keep_away = true;
  hold(g == core::Gate::kHoldZero, moving);
  return g == core::Gate::kAllow;
}

void GameController::clamp(int8_t& v, int8_t& w) const {
  const int8_t m = absI8(v) > absI8(w) ? absI8(v) : absI8(w);
  if (m_cap == 0 || m <= static_cast<int8_t>(m_cap)) return;
  v = static_cast<int8_t>(static_cast<int16_t>(v) * m_cap / m);
  w = static_cast<int8_t>(static_cast<int16_t>(w) * m_cap / m);
}

void GameController::hold(bool held, bool moving) {
  if (!held || !moving) {
    m_holding = false;
    return;
  }
  if (!m_holding) m_hold_since = m_now;
  m_holding = true;
}

// Review I7: the learned roll + the speed x (a reaction time + how old the person estimate already is).
uint16_t GameController::stopCm(int8_t v) const {
  const uint32_t speed_x10 = static_cast<uint32_t>(absI8(v)) * kTopCmsX10 / 100;  // cm/s x 10
  const uint32_t ms = kReactionMs + m_mode.estimateAge(m_now);
  const uint16_t roll = m_h.roll_cm != nullptr ? m_h.roll_cm() : 0;
  return static_cast<uint16_t>(roll + speed_x10 * ms / 10000);
}

// R34.2 (owner: "allow via the map"): a game's backward move asks the engine once; until yes the tracks hold. A
// skill's own backing (the engine's way out) is the engine's to check; the program's BACK and the stick ask.
bool GameController::backAllowed(proto::Source src, uint32_t now) {
  switch (m_back) {
    case Back::kYes: return true;
    case Back::kNo:
      m_keep_away = true;
      m_back = Back::kDone;
      return false;
    case Back::kDone: return false;
    case Back::kAsking:
      backTimeout(now);
      return false;
    default: break;
  }
  if (m_back_cm <= 0 && src != proto::Source::kHuman) return true;  // a skill's drive: not ours to ask
  const int16_t cm = m_back_cm > 0 ? m_back_cm : kBackDefaultCm;
  m_back_cm = 0;
  m_back_id = m_h.ask_back != nullptr ? m_h.ask_back(cm) : -1;
  m_back = m_back_id >= 0 ? Back::kAsking : Back::kNo;
  m_back_at = now;
  return false;
}

void GameController::backTimeout(uint32_t now) {
  if (m_back != Back::kAsking || now - m_back_at <= kBackAnswerMs) return;
  m_back = Back::kDone;
  m_keep_away = true;
}

}  // namespace app
}  // namespace cq
