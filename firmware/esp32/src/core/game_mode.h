// Game mode (bridge LLD R34, Spec 10 S1 / S3 / S8-S12): a program the checker marks as a game runs inside it. Off →
// Arming (the child-play envelope sent, nothing moves until the Uno acks it) → Playing ⇄ Caught (the person within
// 30 cm, a held track or pushing-not-moving: everything stops, one beep; released when seen beyond 50 cm for 2 s);
// any → Held when the person estimate stops coming (1 s) or the engine is gone - only a person's STOP leaves Held.
// Pure (no ports, the caller's clock), host-tested.
#pragma once

#include <stdint.h>

#include "core/keep_out.h"

namespace cq {
namespace core {

enum class GameState : uint8_t { kOff, kArming, kPlaying, kCaught, kHeld };

class GameMode {
 public:
  enum class Start : uint8_t { kGo, kNoCap, kNoPeople, kHeld };
  // Other acked requests may queue ahead of the ENVELOPE (600 ms backstop each): 1.5 s (final review minor).
  static constexpr uint32_t kArmAckMs = 1500;
  static constexpr uint32_t kStaleMs = 300;  // the deadman: an older estimate holds the tracks at 0
  static constexpr uint32_t kHeldMs = 1000;  // no estimate this long: held until STOP
  static constexpr int16_t kReleaseCm = 50;  // the person must be beyond this ...
  static constexpr uint32_t kReleaseMs = 2000;  // ... this long to release a catch

  GameState state() const { return m_state; }
  // A run starts. Held: kHeld (only a STOP leaves it - final review I2). Not a game: kGo, nothing changes. A game:
  // kNoPeople without the engine's seeing-test pass, kNoCap without a measured speed cap; else Arming.
  Start startRun(bool game, bool people, uint8_t cap_pct, uint32_t now_ms);
  Start lastRefusal() const { return m_refusal; }  // why Arming ended Off (no ack: kNoCap)
  uint8_t capPct() const { return m_cap; }
  void onEnvelopeAck(uint32_t now_ms);
  void onEnvelopeFailed();  // the game's ENVELOPE refused or unanswered while arming: Off (7060) - review C2
  // An estimate counts by its own time (e.at_ms): older than kStaleMs on arrival it is dropped (review I1).
  void onPerson(const PersonEst& e, uint32_t now_ms);
  void onCaughtEvent(uint32_t now_ms);  // the Uno's stall, pushing but not moving
  void onEngineGone(uint32_t now_ms);
  // A STOP: back to Off - but Held only for a person's press (S12; design-notes §232: the drive page's own STOP when
  // a thumb leaves the stick must not release it). Every STOP still stops the motors (DriveController).
  void onStop(bool press);
  void onRunEnd();   // the program ended: Off - except Held, which waits for STOP
  bool onUnoHello(uint32_t now_ms);  // the Uno restarted (its envelope lost): true = arm again (a new window)
  Gate gate(const Move& m, uint16_t stop_cm, uint32_t now_ms);
  void step(uint32_t now_ms);  // the timeouts
  bool caught() const { return m_state == GameState::kCaught; }
  uint16_t caughtCount() const { return m_caught_n; }
  int16_t lastDistance() const { return m_has_person ? m_p.d_cm : -1; }
  uint32_t estimateAge(uint32_t now_ms) const { return m_has_person ? now_ms - m_p.at_ms : 0; }
  bool wantsEnvelope(bool& on);  // one-shot: an ENVELOPE to send now (on: the game's, off: cleared)
  bool justCaught();             // one-shot: just caught - end every primitive, beep

 private:
  bool active() const { return m_state != GameState::kOff; }
  void toOff();
  void toHeld();
  void catchNow();
  void releaseCheck(uint32_t now_ms);
  void noteSeen(bool seen, uint32_t now_ms);
  void noteFar(bool far, uint32_t now_ms);
  uint32_t lastHeard() const { return m_has_person ? m_person_rx : m_ack_ms; }

  GameState m_state = GameState::kOff;
  Start m_refusal = Start::kGo;
  uint8_t m_cap = 0;
  uint32_t m_arm_ms = 0, m_ack_ms = 0;
  PersonEst m_p;
  bool m_has_person = false;
  uint32_t m_person_rx = 0;
  uint32_t m_unseen_since = 0;
  bool m_unseen = false;
  uint32_t m_far_since = 0;
  bool m_far = false;
  uint16_t m_caught_n = 0;
  bool m_env_pending = false, m_env_on = false;
  bool m_just_caught = false;
};

}  // namespace core
}  // namespace cq
