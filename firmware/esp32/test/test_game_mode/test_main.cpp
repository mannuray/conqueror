// GameMode and KeepOut (bridge LLD R34): the people games' safety on the bridge - arming, the keep-out on every move,
// caught, held. Pure, host-tested.
#include <unity.h>

#include "core/game_mode.h"
#include "core/keep_out.h"

using cq::core::GameMode;
using cq::core::GameState;
using cq::core::Gate;
using cq::core::KeepOut;
using cq::core::Move;
using cq::core::PersonEst;

void setUp() {}
void tearDown() {}

namespace {
constexpr Move kForward{30, 0.0f, false};
constexpr Move kTurn{0, 60.0f, true};
constexpr Move kBack{-30, 0.0f, false};

PersonEst at(int16_t d_cm, int16_t bearing = 0, bool seen = true) {
  return PersonEst{seen, d_cm, bearing, 0, 200, 0};
}

// An estimate made at `now` (the engine's at_ms), handed to the game at `now`.
void see(GameMode& g, uint32_t now, int16_t d_cm, int16_t bearing = 0, bool seen = true) {
  PersonEst e = at(d_cm, bearing, seen);
  e.at_ms = now;
  g.onPerson(e, now);
}

// A game armed and playing at t = 100 ms (the envelope acked).
GameMode playing() {
  GameMode g;
  g.startRun(true, true, 75, 0);
  g.onEnvelopeAck(100);
  return g;
}
}  // namespace

// ---- GameMode

// design-notes §243: the Uno's answer handled later in the loop, stamped 1 ms after the loop's now.
void test_an_ack_stamped_after_the_loops_now_does_not_hold() {
  GameMode g;
  g.startRun(true, true, 75, 1000);
  g.onEnvelopeAck(1001);
  g.step(1000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
  see(g, 1002, 120);
  g.step(1001);  // an estimate from later in the loop: fresh, not stale
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kBack, 20, 1001)));
}

void test_a_non_game_run_never_arms() {
  GameMode g;
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kGo), static_cast<int>(g.startRun(false, false, 0, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 10)));
  bool on = false;
  TEST_ASSERT_FALSE(g.wantsEnvelope(on));
}

void test_no_cap_or_no_people_refuses() {
  GameMode g;
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kNoCap), static_cast<int>(g.startRun(true, true, 0, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kNoPeople), static_cast<int>(g.startRun(true, false, 75, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  bool on = false;
  TEST_ASSERT_FALSE(g.wantsEnvelope(on));
}

void test_arming_holds_until_the_envelope_ack() {
  GameMode g;
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kGo), static_cast<int>(g.startRun(true, true, 75, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kArming), static_cast<int>(g.state()));
  bool on = false;
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_TRUE(on);
  TEST_ASSERT_FALSE(g.wantsEnvelope(on));  // one-shot
  see(g, 50, 200);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 60)));
  g.onEnvelopeAck(100);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_no_ack_within_the_arm_window_ends_off_as_no_cap() {
  GameMode g;
  g.startRun(true, true, 75, 0);
  g.step(GameMode::kArmAckMs);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kArming), static_cast<int>(g.state()));
  g.step(GameMode::kArmAckMs + 1);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kNoCap), static_cast<int>(g.lastRefusal()));
}

void test_caught_at_10_and_released_after_30_for_2_s() {  // design-notes §248
  GameMode g = playing();
  see(g, 150, 11);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
  see(g, 200, 10);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.caught());
  TEST_ASSERT_TRUE(g.justCaught());
  TEST_ASSERT_FALSE(g.justCaught());  // one-shot
  TEST_ASSERT_EQUAL_UINT16(1, g.caughtCount());
  see(g, 500, 25);  // still within reach
  see(g, 1000, 30);
  see(g, 2999, 30);
  g.step(2999);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  g.step(3000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_a_step_back_inside_30_restarts_the_2_s() {
  GameMode g = playing();
  see(g, 200, 5);
  see(g, 1000, 35);
  see(g, 2000, 28);  // back inside 30 cm
  for (uint32_t t = 2100; t <= 3500; t += 200) see(g, t, 35);  // estimates keep coming (else: held)
  g.step(3500);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  for (uint32_t t = 3700; t <= 4100; t += 200) see(g, t, 35);
  g.step(4100);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_stall_or_pushing_not_moving_is_caught() {
  GameMode g = playing();
  see(g, 150, 120);
  g.onCaughtEvent(200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.justCaught());
}

void test_stale_300_ms_holds_1_s_is_held() {
  GameMode g = playing();
  see(g, 0, 200, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 299)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 301)));
  g.step(1001);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  see(g, 1100, 200);  // a person again does not leave Held
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  g.onRunEnd();  // nor the run ending (S12: only a person's STOP)
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  bool on = true;
  while (g.wantsEnvelope(on)) {
  }
  g.onStop(true);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_FALSE(on);  // the envelope cleared
}

// Tank check 2026-11-10 (design-notes §232): the drive page's own STOP when a thumb leaves the stick released Held.
// S12: only a person pressing STOP does; every other STOP still stops the motors, game mode stays Held.
void test_only_a_pressed_stop_releases_held() {
  GameMode g = playing();
  see(g, 0, 200, 0);
  g.step(1001);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  g.onStop(false);  // a thumb lifted, a mode switch, a page hidden
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  g.onStop(true);  // the STOP button
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
}

void test_no_person_at_all_after_arming_is_held_after_1_s() {
  GameMode g = playing();  // acked at 100, no estimate yet
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 150)));
  g.step(1100);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
  g.step(1101);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
}

void test_engine_gone_is_held() {
  GameMode a;
  a.startRun(true, true, 75, 0);
  a.onEngineGone(10);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(a.state()));
  GameMode b = playing();
  b.onEngineGone(200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(b.state()));
  GameMode c = playing();
  see(c, 150, 20);
  c.onEngineGone(200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(c.state()));
  GameMode off;
  off.onEngineGone(10);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(off.state()));
}

void test_uno_hello_rearms() {
  GameMode g = playing();
  bool on = false;
  while (g.wantsEnvelope(on)) {
  }
  TEST_ASSERT_TRUE(g.onUnoHello(150));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kArming), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_TRUE(on);
  see(g, 150, 200);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 160)));
  g.onEnvelopeAck(170);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 180)));
  GameMode off;
  TEST_ASSERT_FALSE(off.onUnoHello(10));
}

void test_review_focus_4_person_then_gate_same_tick() {
  GameMode g = playing();
  see(g, 150, 80);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 150)));
  see(g, 160, 25);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 160)));
}

void test_stop_while_arming() {
  GameMode g;
  g.startRun(true, true, 75, 0);
  bool on = false;
  while (g.wantsEnvelope(on)) {
  }
  g.onStop(true);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_FALSE(on);
}

void test_the_run_ending_clears_the_envelope() {
  GameMode g = playing();
  bool on = false;
  while (g.wantsEnvelope(on)) {
  }
  g.onRunEnd();
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_FALSE(on);
}

// ---- design-notes §248: two levels while caught, a not-found release, only sightings catch

// Caught at `t` by a sighting at `d` cm, the sonar clear (no echo).
GameMode caughtAt(uint32_t t, int16_t d) {
  GameMode g = playing();
  g.onSonar(65535);
  see(g, t, d);
  return g;
}

void test_an_unseen_estimate_never_catches() {
  GameMode g = playing();
  see(g, 200, 5, 0, false);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_caught_turns_on_the_spot_only_seen_fresh_beyond_15() {
  GameMode g = caughtAt(200, 8);
  see(g, 300, 16);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kTurn, 20, 300)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 300)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kBack, 20, 300)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kTurn, 20, 601)));  // stale
  see(g, 700, 15);  // the absolute level's edge: the tracks stay stopped
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kTurn, 20, 700)));
  see(g, 800, 40, 0, false);  // out of view
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kTurn, 20, 800)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
}

// Seen at 20 after the catch, then out of view: released after the 3 s sweep, and no catch again while unseen.
void test_not_found_for_the_sweep_releases() {
  GameMode g = caughtAt(200, 8);
  see(g, 400, 20);
  for (uint32_t t = 500; t <= 3400; t += 100) see(g, t, 20, 0, false);
  g.step(3499);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  see(g, 3500, 20, 0, false);
  g.step(3500);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
  see(g, 3600, 5, 0, false);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_not_found_never_releases_last_seen_inside_10_an_echo_or_a_touch() {
  GameMode near = caughtAt(200, 8);  // gone straight after the catch at 8
  for (uint32_t t = 300; t <= 4000; t += 100) see(near, t, 8, 0, false);
  near.step(4000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(near.state()));
  GameMode echo = caughtAt(200, 8);
  see(echo, 300, 20);
  echo.onSonar(290);  // something 29 cm ahead
  for (uint32_t t = 400; t <= 4000; t += 100) see(echo, t, 20, 0, false);
  echo.step(4000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(echo.state()));
  echo.onSonar(65534);  // a sonar fault is not clear either
  echo.step(4000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(echo.state()));
  GameMode touch = playing();
  touch.onSonar(65535);
  see(touch, 150, 60);
  touch.onCaughtEvent(200);
  see(touch, 300, 20);
  for (uint32_t t = 400; t <= 4000; t += 100) see(touch, t, 20, 0, false);
  touch.step(4000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(touch.state()));
  for (uint32_t t = 4100; t <= 6200; t += 100) see(touch, t, 35);  // seen clear: released
  touch.step(6200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(touch.state()));
}

void test_a_blind_turn_only_last_seen_from_30_with_the_sonar_clear() {
  GameMode g = playing();
  g.onSonar(65535);
  see(g, 150, 30);
  see(g, 200, 30, 0, false);
  see(g, 1300, 30, 0, false);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kTurn, 20, 1300)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 1300)));
  g.onSonar(290);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kTurn, 20, 1300)));
  GameMode n = playing();
  n.onSonar(65535);
  see(n, 150, 29);
  see(n, 200, 29, 0, false);
  see(n, 1300, 29, 0, false);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(n.gate(kTurn, 20, 1300)));
}

// ---- final review fixes

void test_a_late_rearm_restarts_the_arm_window() {  // C1
  GameMode g = playing();
  see(g, 4990, 200);
  TEST_ASSERT_TRUE(g.onUnoHello(5000));
  g.step(5000 + GameMode::kArmAckMs);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kArming), static_cast<int>(g.state()));
  g.onEnvelopeAck(5000 + GameMode::kArmAckMs);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_an_old_estimate_is_not_fresh() {  // I1: the estimate's own age counts, not when it arrived
  GameMode g = playing();
  PersonEst old = at(200);
  old.at_ms = 600;  // 400 ms old when it arrives at 1000
  g.onPerson(old, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 1000)));
  PersonEst fresh = at(200);
  fresh.at_ms = 1100;
  g.onPerson(fresh, 1150);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 1150)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 1401)));  // 301 ms old
  PersonEst stuck = at(200);  // a frozen camera resending its last estimate
  stuck.at_ms = 1100;
  for (uint32_t t = 1200; t <= 2200; t += 100) g.onPerson(stuck, t);
  g.step(2200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
}

void test_a_run_start_while_held_is_refused() {  // I2
  GameMode g = playing();
  g.onEngineGone(200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kHeld), static_cast<int>(g.startRun(true, true, 75, 300)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kHeld), static_cast<int>(g.startRun(false, true, 75, 300)));
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  g.onStop(true);
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kGo), static_cast<int>(g.startRun(false, true, 75, 400)));
}

void test_a_failed_envelope_while_arming_ends_off() {  // C2
  GameMode g;
  g.startRun(true, true, 75, 0);
  g.onEnvelopeFailed();
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kNoCap), static_cast<int>(g.lastRefusal()));
  GameMode p = playing();
  p.onEnvelopeFailed();  // only while arming
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(p.state()));
}

void test_the_last_distance_for_the_pages() {
  GameMode g = playing();
  see(g, 150, 85);
  TEST_ASSERT_EQUAL_INT16(85, g.lastDistance());
}

// ---- KeepOut

void test_forward_towards_ends_kept_away() {
  // stopping 20 + margin 5 + caught 10 (design-notes §248)
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(36), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kEndKeptAway), static_cast<int>(KeepOut::check(at(35), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kEndKeptAway),
                    static_cast<int>(KeepOut::check(at(35, 59), kForward, 20, 0)));  // within ±60° of ahead
}

void test_forward_away_or_aside_allowed() {
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, 90), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, -61), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, 180), kForward, 20, 0)));
}

void test_turns_from_15_cm() {  // design-notes §248
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(16), kTurn, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(KeepOut::check(at(15), kTurn, 20, 0)));
}

void test_unseen_1_s_holds_forward_and_turns() {
  const PersonEst gone = at(150, 0, false);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(150, 90, false), kForward, 20, 1000)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(KeepOut::check(gone, kForward, 20, 1001)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(KeepOut::check(gone, kTurn, 20, 1001)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(gone, kBack, 20, 1001)));
}

void test_backward_is_never_cut_by_bearing() {
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, 0), kBack, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, 180), kBack, 20, 0)));
}

void test_standing_still_is_always_allowed() {
  const Move still{0, 0.0f, false};
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(31), still, 20, 5000)));
}

// ---- withSonar (R34.11, design-notes §235): the sonar minus a hand allowance, ahead only

void test_sonar_ahead_takes_the_nearer_with_the_hand_allowance() {
  TEST_ASSERT_EQUAL_INT16(25, cq::core::withSonar(at(60), 450).d_cm);   // the camera missed the arms
  TEST_ASSERT_EQUAL_INT16(20, cq::core::withSonar(at(20), 450).d_cm);   // the camera nearer: its own
  TEST_ASSERT_EQUAL_INT16(10, cq::core::withSonar(at(60, 12), 300).d_cm);  // the beam's edge still counts
  TEST_ASSERT_EQUAL_INT16(0, cq::core::withSonar(at(60), 120).d_cm);    // never below 0
}

// design-notes §246: an echo more than 50 cm nearer than the camera is something in between (a table leg), not them
void test_an_echo_far_nearer_than_the_camera_is_not_the_person() {
  TEST_ASSERT_EQUAL_INT16(100, cq::core::withSonar(at(100), 360).d_cm);  // 64 cm nearer: the leg
  TEST_ASSERT_EQUAL_INT16(30, cq::core::withSonar(at(100), 500).d_cm);   // 50 cm nearer: still them
}

void test_sonar_aside_unseen_or_no_echo_leaves_the_camera() {
  TEST_ASSERT_EQUAL_INT16(60, cq::core::withSonar(at(60, 30), 450).d_cm);   // outside the beam
  TEST_ASSERT_EQUAL_INT16(60, cq::core::withSonar(at(60, -13), 450).d_cm);
  TEST_ASSERT_EQUAL_INT16(60, cq::core::withSonar(at(60), 65535).d_cm);     // no echo
  TEST_ASSERT_EQUAL_INT16(60, cq::core::withSonar(at(60), 65534).d_cm);     // sensor fault
  TEST_ASSERT_EQUAL_INT16(-1, cq::core::withSonar(at(-1, 0, false), 450).d_cm);  // not seen: unchanged
}

void test_sonar_gives_a_distance_when_the_camera_has_none() {
  TEST_ASSERT_EQUAL_INT16(40, cq::core::withSonar(at(-1), 600).d_cm);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_ack_stamped_after_the_loops_now_does_not_hold);
  RUN_TEST(test_a_non_game_run_never_arms);
  RUN_TEST(test_no_cap_or_no_people_refuses);
  RUN_TEST(test_arming_holds_until_the_envelope_ack);
  RUN_TEST(test_no_ack_within_the_arm_window_ends_off_as_no_cap);
  RUN_TEST(test_caught_at_10_and_released_after_30_for_2_s);
  RUN_TEST(test_a_step_back_inside_30_restarts_the_2_s);
  RUN_TEST(test_an_unseen_estimate_never_catches);
  RUN_TEST(test_caught_turns_on_the_spot_only_seen_fresh_beyond_15);
  RUN_TEST(test_not_found_for_the_sweep_releases);
  RUN_TEST(test_not_found_never_releases_last_seen_inside_10_an_echo_or_a_touch);
  RUN_TEST(test_a_blind_turn_only_last_seen_from_30_with_the_sonar_clear);
  RUN_TEST(test_stall_or_pushing_not_moving_is_caught);
  RUN_TEST(test_stale_300_ms_holds_1_s_is_held);
  RUN_TEST(test_only_a_pressed_stop_releases_held);
  RUN_TEST(test_no_person_at_all_after_arming_is_held_after_1_s);
  RUN_TEST(test_engine_gone_is_held);
  RUN_TEST(test_uno_hello_rearms);
  RUN_TEST(test_review_focus_4_person_then_gate_same_tick);
  RUN_TEST(test_stop_while_arming);
  RUN_TEST(test_the_run_ending_clears_the_envelope);
  RUN_TEST(test_a_late_rearm_restarts_the_arm_window);
  RUN_TEST(test_an_old_estimate_is_not_fresh);
  RUN_TEST(test_a_run_start_while_held_is_refused);
  RUN_TEST(test_a_failed_envelope_while_arming_ends_off);
  RUN_TEST(test_the_last_distance_for_the_pages);
  RUN_TEST(test_forward_towards_ends_kept_away);
  RUN_TEST(test_forward_away_or_aside_allowed);
  RUN_TEST(test_turns_from_15_cm);
  RUN_TEST(test_unseen_1_s_holds_forward_and_turns);
  RUN_TEST(test_backward_is_never_cut_by_bearing);
  RUN_TEST(test_standing_still_is_always_allowed);
  RUN_TEST(test_sonar_ahead_takes_the_nearer_with_the_hand_allowance);
  RUN_TEST(test_an_echo_far_nearer_than_the_camera_is_not_the_person);
  RUN_TEST(test_sonar_aside_unseen_or_no_echo_leaves_the_camera);
  RUN_TEST(test_sonar_gives_a_distance_when_the_camera_has_none);
  return UNITY_END();
}
