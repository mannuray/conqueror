#include "app/game_controller.h"

namespace cq {
namespace app {

uint16_t GameController::onRunStart(bool game, uint32_t now) {
  const bool people = m_h.people != nullptr && m_h.people();
  switch (m_mode.startRun(game, people, m_cap, now)) {
    case core::GameMode::Start::kNoPeople: return 7061;
    case core::GameMode::Start::kNoCap: return 7060;
    default: break;
  }
  m_back = Back::kNone;
  m_back_cm = 0;
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

void GameController::onBodyOnline(uint32_t) {
  if (m_mode.onUnoHello()) return;  // a game: armed again (GameMode wants the envelope)
  if (m_mode.state() == core::GameState::kOff) {  // boot, or an Uno restart with no game: never a stale cap
    m_env_due = true;
    m_env_on = false;
  }
}

void GameController::onEnvelopeDone(uint8_t status, uint32_t now) {
  if (status == 0) m_mode.onEnvelopeAck(now);  // no answer / refused: Arming times out (7060)
}

void GameController::onAnswer(int32_t id, int32_t value, uint32_t now) {
  if (m_back != Back::kAsking || id != m_back_id || now - m_back_at > kBackAnswerMs) return;
  m_back = value != 0 ? Back::kYes : Back::kNo;
}

void GameController::tick(uint32_t now) {
  m_now = now;
  core::PersonEst e;
  if (m_h.take_person != nullptr && m_h.take_person(e)) m_mode.onPerson(e, now);
  if (m_mode.state() == core::GameState::kArming) m_was_arming = true;
  m_mode.step(now);
  backTimeout(now);  // no answer in time: the backward move ends now, not at its next DRIVE
  envelope(now);
  if (m_mode.justCaught() && m_h.caught != nullptr) m_h.caught(now);
  if (m_keep_away) {
    m_keep_away = false;
    if (m_h.keep_away != nullptr) m_h.keep_away(now);
  }
  transitions(now);
}

// The envelope GameMode wants (its own, or cleared) and the boot clear, retried while the queue is full.
void GameController::envelope(uint32_t) {
  bool on = false;
  if (m_mode.wantsEnvelope(on)) {
    m_env_due = true;
    m_env_on = on;
  }
  if (!m_env_due || m_h.queue_envelope == nullptr) return;
  if (m_h.queue_envelope(m_env_on ? m_cap : 100, 0, m_env_on ? kChildPlay : 0)) m_env_due = false;
}

// A state changed: the pages hear it; Held and a failed arming end the run; the engine's stream follows the game.
void GameController::transitions(uint32_t now) {
  const core::GameState s = m_mode.state();
  engine(s != core::GameState::kOff);
  if (s == m_last) return;
  m_last = s;
  if (m_h.event != nullptr) m_h.event(s, 0, m_mode.caughtCount());
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

bool GameController::gate(void* ctx, int8_t v, int8_t w, proto::Source) {
  return ctx == nullptr || static_cast<GameController*>(ctx)->allows(v, w);
}

bool GameController::allows(int8_t v, int8_t w) {
  if (v >= 0) m_back = Back::kNone;  // a backward move ended: the next one asks again
  if (v < 0 && m_mode.state() == core::GameState::kPlaying && !backAllowed(m_now)) return false;
  const core::Move m{v, static_cast<float>(w), v == 0 && w != 0};
  const uint16_t stop = m_h.stop_cm != nullptr ? m_h.stop_cm() : 0;
  const core::Gate g = m_mode.gate(m, stop, m_now);
  if (g == core::Gate::kEndKeptAway) m_keep_away = true;
  return g == core::Gate::kAllow;
}

void GameController::backTimeout(uint32_t now) {
  if (m_back != Back::kAsking || now - m_back_at <= kBackAnswerMs) return;
  m_back = Back::kDone;
  m_keep_away = true;
}

// R34.2 (owner: "allow via the map"): a game's backward move asks the engine once; until yes the tracks hold.
bool GameController::backAllowed(uint32_t now) {
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
  const int16_t cm = m_back_cm > 0 ? m_back_cm : kBackDefaultCm;
  m_back_cm = 0;
  m_back_id = m_h.ask_back != nullptr ? m_h.ask_back(cm) : -1;
  m_back = m_back_id >= 0 ? Back::kAsking : Back::kNo;
  m_back_at = now;
  return false;
}

}  // namespace app
}  // namespace cq
