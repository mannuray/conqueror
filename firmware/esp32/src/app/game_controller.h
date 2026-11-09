// Game mode joined to the bridge (bridge LLD R34): a game run's start (the seeing-test lock, the measured speed cap)
// and end, the child-play envelope through the Uno's acked queue, the engine's person stream, caught (every motion
// ends, a beep), held (the run ends 7052, nothing moves until a person's STOP), back_ok (a backward move in a game asks
// the engine's map first) and the gate every DRIVE asks (BodyLink::setDriveGate). Link task only; captureless hooks.
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
    bool (*take_person)(core::PersonEst& out) = nullptr;  // the newest estimate, one-shot (SkillHub::takePerson)
    void (*send_game)(bool on) = nullptr;                // {"t":"game","on"} to the engine
    bool (*queue_envelope)(uint8_t cap, uint8_t accel, uint8_t lock) = nullptr;  // false: the queue is full
    void (*keep_away)(uint32_t now) = nullptr;           // end the running primitive normally ("kept away")
    void (*caught)(uint32_t now) = nullptr;              // end every primitive and the lease ("caught"), beep
    void (*end_run)(uint16_t code, uint32_t now) = nullptr;  // end the running program with this code
    int32_t (*ask_back)(int16_t cm) = nullptr;           // skill_ask back_ok: its id, −1 with no engine
    void (*event)(core::GameState s, int16_t d_cm, uint16_t caught) = nullptr;  // to the pages
    uint16_t (*stop_cm)() = nullptr;                     // how far the tank runs on after a stop (the learned roll)
  };
  static constexpr uint8_t kChildPlay = 1u << 1;      // EnvelopeFlagBit::kChildPlay
  static constexpr int16_t kBackDefaultCm = 30;       // a backward DRIVE whose length nobody gave
  static constexpr uint32_t kBackAnswerMs = 300;      // as every question (R26.1)

  void setHooks(const Hooks& h) { m_h = h; }
  void setCap(uint8_t pct) { m_cap = pct; }
  uint8_t cap() const { return m_cap; }
  // A run starts: 0 to go, 7061 (no seeing test), 7060 (no speed cap). A game then arms before its first motion.
  uint16_t onRunStart(bool game, uint32_t now);
  bool ready() const { return m_mode.state() != core::GameState::kArming; }  // false: the program waits
  bool caught() const { return m_mode.caught(); }
  core::GameState state() const { return m_mode.state(); }
  void onRunEnd(uint32_t now);
  void onStop(uint32_t now);
  void onEngineGone(uint32_t now) { m_mode.onEngineGone(now); }
  void onBodyOnline(uint32_t now);  // the Uno (re)started: a game arms again; off, the envelope is cleared
  void onCaughtEvent(uint32_t now) { m_mode.onCaughtEvent(now); }  // a held track, pushing but not moving
  void onEnvelopeDone(uint8_t status, uint32_t now);
  void onAnswer(int32_t id, int32_t value, uint32_t now);  // the engine's skill_answer to back_ok
  void backing(int16_t cm) { m_back_cm = cm; }             // the next backward move's length (Logo's BACK n)
  void tick(uint32_t now);
  static bool gate(void* ctx, int8_t v, int8_t w, proto::Source src);  // BodyLink's DriveGateFn

 private:
  enum class Back : uint8_t { kNone, kAsking, kYes, kNo, kDone };  // kDone: refused and its move already ended
  bool allows(int8_t v, int8_t w);
  bool backAllowed(uint32_t now);
  void backTimeout(uint32_t now);
  void envelope(uint32_t now);
  void transitions(uint32_t now);
  void engine(bool on);

  core::GameMode m_mode;
  Hooks m_h;
  uint8_t m_cap = 0;
  uint32_t m_now = 0;
  bool m_keep_away = false;
  bool m_env_due = false, m_env_on = false;
  bool m_game_on = false;
  bool m_was_arming = false;
  core::GameState m_last = core::GameState::kOff;
  Back m_back = Back::kNone;
  int32_t m_back_id = -1;
  uint32_t m_back_at = 0;
  int16_t m_back_cm = 0;
};

}  // namespace app
}  // namespace cq
