// Game mode (bridge LLD R34, Spec 10 S1 / S3 / S8-S12): a program the checker marks as a game runs inside it. Off →
// Arming (the child-play envelope sent, nothing moves until the Uno acks it) → Playing ⇄ Caught (the person seen within
// 10 cm, a held track or pushing-not-moving: the tracks stop, one beep; only a turn on the spot with the person seen
// beyond 15 cm; released when seen beyond 30 cm for 2 s, or not found through the 3 s sweep - R34.14, §248);
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
  static constexpr int16_t kReleaseCm = 30;  // the person must be beyond this ...
  static constexpr uint32_t kReleaseMs = 2000;  // ... this long to release a catch
  static constexpr uint32_t kSweepMs = 3000;   // or out of view this long (FACE's camera sweep, R34.14) ...
  static constexpr int16_t kClearCm = 30;      // ... with no echo ahead inside this
  static constexpr int16_t kBlindTurnCm = 30;  // out of view: a turn on the spot only when last seen this far

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
  void onSonar(uint16_t mm) { m_sonar_mm = mm; }  // the sonar ahead (65535 no echo, 65534 a fault)
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
  uint32_t estimateAge(uint32_t now_ms) const { return m_has_person ? ago(now_ms, m_p.at_ms) : 0; }
  bool wantsEnvelope(bool& on);  // one-shot: an ENVELOPE to send now (on: the game's, off: cleared)
  bool justCaught();             // one-shot: just caught - end every primitive, beep

 private:
  bool active() const { return m_state != GameState::kOff; }
  void toOff();
  void toHeld();
  void catchNow();
  void releaseCheck(uint32_t now_ms);
  bool notFound(uint32_t now_ms) const;
  bool aheadClear() const;
  Gate caughtGate(const Move& m) const;
  Gate playGate(const Move& m, uint16_t stop_cm, uint32_t now_ms) const;
  void noteSeen(bool seen, uint32_t now_ms);
  void sighting(int16_t d_cm);
  void noteFar(bool far, uint32_t now_ms);
  uint32_t lastHeard() const { return m_has_person ? m_person_rx : m_ack_ms; }
  // How long ago `then` was; a time after `now` (an answer handled later in the same loop with its own millis())
  // is 0 ms ago, never ~49 days (design-notes §243, R34.12).
  static uint32_t ago(uint32_t now_ms, uint32_t then_ms) {
    const uint32_t d = now_ms - then_ms;
    return static_cast<int32_t>(d) < 0 ? 0 : d;
  }

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
  int16_t m_seen_cm = -1;  // the last seen distance (a sighting, not an out-of-view estimate)
  bool m_touch = false;    // this catch was a touch: only a sighting releases it
  uint16_t m_sonar_mm = 0;  // no reading yet: not clear
  bool m_env_pending = false, m_env_on = false;
  bool m_just_caught = false;
};

}  // namespace core
}  // namespace cq
