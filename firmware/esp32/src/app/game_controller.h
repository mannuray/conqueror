// Game mode joined to the bridge (bridge LLD R34): a game run's start (the seeing-test lock, the measured speed cap)
// and end, the child-play envelope through the Uno's acked queue, the engine's person estimates, caught (every motion
// ends, a beep), held (the run ends 7052, nothing moves until a STOP), back_ok (a backward move in a game asks the
// engine's map first) and the gate every DRIVE asks (BodyLink::setDriveGate): the keep-out, the game's speed cap, and
// a hold longer than kHoldEndMs ends the move "kept away". Link task only; captureless hooks.
#pragma once

#include <stdint.h>

#include "core/game_mode.h"
#include "cq_protocol.h"

namespace cq {
namespace app {

class GameController {
 public:
  struct Hooks {
    bool (*people)() = nullptr;                          // the engine's seeing test passed (SkillHub::people)
    void (*send_game)(bool on) = nullptr;                // {"t":"game","on"} to the engine
    bool (*queue_envelope)(uint8_t cap, uint8_t accel, uint8_t lock) = nullptr;  // false: the queue is full
    void (*keep_away)(uint32_t now) = nullptr;           // end the running primitive normally ("kept away")
    void (*caught)(uint32_t now) = nullptr;              // end every primitive and the lease ("caught"), beep
    void (*end_run)(uint16_t code, uint32_t now) = nullptr;  // end the running program with this code
    int32_t (*ask_back)(int16_t cm) = nullptr;           // skill_ask back_ok: its id, −1 with no engine
    void (*event)(core::GameState s, int16_t d_cm, uint16_t caught) = nullptr;  // to the pages
    uint16_t (*roll_cm)() = nullptr;                     // the learned roll-on after a stop (cm)
  };
  static constexpr uint8_t kChildPlay = 1u << 1;      // EnvelopeFlagBit::kChildPlay
  static constexpr int16_t kBackDefaultCm = 30;       // a backward DRIVE whose length nobody gave
  static constexpr uint32_t kBackAnswerMs = 300;      // as every question (R26.1)
  static constexpr uint32_t kBackYesMs = 3000;        // one yes covers one backward move, at most this long
  static constexpr uint32_t kHoldEndMs = 300;         // a longer hold ends the move (the motion's watches, review I4)
  static constexpr uint32_t kReactionMs = 300;        // the stopping distance's reaction time (review I7)
  static constexpr uint16_t kTopCmsX10 = 335;         // the tank's measured top speed, 33.5 cm/s (design-notes §69)

  void setHooks(const Hooks& h) { m_h = h; }
  void setCap(uint8_t pct) { m_cap = pct; }
  uint8_t cap() const { return m_cap; }
  // A run starts: 0 to go, 7052 (held: STOP first), 7061 (no seeing test), 7060 (no speed cap).
  uint16_t onRunStart(bool game, uint32_t now);
  bool ready() const { return m_mode.state() != core::GameState::kArming; }  // false: the program waits
  bool caught() const { return m_mode.caught(); }
  core::GameState state() const { return m_mode.state(); }
  uint16_t caughtCount() const { return m_mode.caughtCount(); }
  uint8_t gameCap() const { return m_mode.state() == core::GameState::kOff ? 0 : m_cap; }  // 0: no game
  void onRunEnd(uint32_t now);
  void onStop(uint32_t now, bool press);  // press: the STOP button (only it releases Held)
  void onEngineGone(uint32_t now) { m_mode.onEngineGone(now); }
  void onBodyOnline(uint32_t now);  // the Uno (re)started: a game arms again; off, the envelope is cleared
  void onCaughtEvent(uint32_t now) { m_mode.onCaughtEvent(now); }  // the Uno's stall: a held track
  void onPerson(const core::PersonEst& e, uint32_t now);  // at receipt (review I6)
  void onEnvelopeDone(uint8_t status, bool on, uint32_t now);  // the answer to our ENVELOPE (on: the game's)
  void onAnswer(int32_t id, int32_t value, uint32_t now);  // the engine's skill_answer to back_ok
  void backing(int16_t cm) { m_back_cm = cm; }             // the next backward move's length (Logo's BACK n)
  void tick(uint32_t now);
  // BodyLink's DriveGateFn: false = send zeros; v / w are clamped to the game's cap.
  static bool gateClamp(void* ctx, int8_t& v, int8_t& w, proto::Source src);
  static bool gate(void* ctx, int8_t v, int8_t w, proto::Source src) { return gateClamp(ctx, v, w, src); }

 private:
  enum class Back : uint8_t { kNone, kAsking, kYes, kNo, kDone };  // kDone: refused and its move already ended
  bool allows(int8_t& v, int8_t& w, proto::Source src);
  bool backAllowed(proto::Source src, uint32_t now);
  void backTimeout(uint32_t now);
  void lapse(uint32_t now);
  void clamp(int8_t& v, int8_t& w) const;
  void hold(bool held, bool moving);
  uint16_t stopCm(int8_t v) const;
  void envelope();
  void transitions(uint32_t now);
  void engine(bool on);

  core::GameMode m_mode;
  Hooks m_h;
  uint8_t m_cap = 0;
  uint32_t m_now = 0;
  bool m_keep_away = false;
  bool m_env_due = false, m_env_on = false;
  uint8_t m_env_queued = 0;  // ENVELOPEs queued, not yet answered (review C2: only the last one's ack counts)
  bool m_env_last_on = false;
  bool m_game_on = false;
  bool m_was_arming = false;
  bool m_holding = false;
  uint32_t m_hold_since = 0;
  core::GameState m_last = core::GameState::kOff;
  Back m_back = Back::kNone;
  int32_t m_back_id = -1;
  uint32_t m_back_at = 0;
  int16_t m_back_cm = 0;
};

}  // namespace app
}  // namespace cq
