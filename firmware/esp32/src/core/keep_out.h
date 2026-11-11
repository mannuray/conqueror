// The people games' keep-out (bridge LLD R34.2, Spec 10 S2 / S4 / S8): one rule for every move the bridge sends during
// a game - never within 30 cm of the person, never driving towards them, no turn closer than 35 cm, and nothing forward
// or turning once the person has been out of sight for 1 s. Pure, host-tested.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

// The engine's estimate of where the person is (the `person` message, AI engine LLD §28.2): d_cm is the pessimistic
// distance from the tank's front edge (unsure = nearer), −1 when it has none; bearing in degrees, + left.
struct PersonEst {
  bool seen = false;
  int16_t d_cm = -1;
  int16_t bearing_deg = 0;
  int8_t coming = 0;  // +1 coming closer, −1 going away
  uint8_t sure = 0;   // 0..255
  uint32_t at_ms = 0;
};

// What to do with a DRIVE: send it, send zeros instead (hold), or end the primitive normally ("kept away").
enum class Gate : uint8_t { kAllow, kHoldZero, kEndKeptAway };

// One move as the bridge is about to send it: v % (+ forward), the yaw rate, and whether it is a turn on the spot.
struct Move {
  int8_t v_pct = 0;
  float yaw_rate_dps = 0.0f;
  bool turn_only = false;
};

class KeepOut {
 public:
  static constexpr int16_t kCaughtCm = 30;     // never closer (owner)
  static constexpr int16_t kTurnCm = 35;       // the front corners sweep ~4 cm nearer while turning
  static constexpr int16_t kMarginCm = 5;      // over the stopping distance
  static constexpr int16_t kAheadDeg = 60;     // "towards": the person within this of ahead
  static constexpr uint32_t kUnseenMs = 1000;  // owner: hold, then look
  static constexpr int16_t kReachCm = 20;      // a crawler's hands ahead of the body the sonar sees (design-notes §235)
  static constexpr int16_t kSonarConeDeg = 12;  // the sonar's beam: a person this near ahead is in it
  // stop_cm: how far the tank runs on after a stop at this speed (the learned coast); unseen_ms: how long the person
  // has been out of sight (0 = seen now).
  static Gate check(const PersonEst& e, const Move& m, uint16_t stop_cm, uint32_t unseen_ms);
};

// R34.11 (design-notes §235): a person seen in the sonar's beam is no farther than the sonar's reading minus the hand
// allowance - the camera misses a close crawler's arms. Otherwise (aside, unseen, no echo / fault) `e` unchanged.
PersonEst withSonar(const PersonEst& e, uint16_t sonar_mm);

}  // namespace core
}  // namespace cq
