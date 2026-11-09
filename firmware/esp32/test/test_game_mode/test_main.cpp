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

// A game armed and playing at t = 100 ms (the envelope acked).
GameMode playing() {
  GameMode g;
  g.startRun(true, true, 75, 0);
  g.onEnvelopeAck(100);
  return g;
}
}  // namespace

// ---- GameMode

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
  g.onPerson(at(200), 50);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 60)));
  g.onEnvelopeAck(100);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_no_ack_within_500_ms_ends_off_as_no_cap() {
  GameMode g;
  g.startRun(true, true, 75, 0);
  g.step(500);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kArming), static_cast<int>(g.state()));
  g.step(501);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_EQUAL(static_cast<int>(GameMode::Start::kNoCap), static_cast<int>(g.lastRefusal()));
}

void test_caught_at_30_and_released_after_50_for_2_s() {
  GameMode g = playing();
  g.onPerson(at(30), 200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.caught());
  TEST_ASSERT_TRUE(g.justCaught());
  TEST_ASSERT_FALSE(g.justCaught());  // one-shot
  TEST_ASSERT_EQUAL_UINT16(1, g.caughtCount());
  g.onPerson(at(45), 500);  // still within reach
  g.onPerson(at(55), 1000);
  g.onPerson(at(55), 2999);
  g.step(2999);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  g.step(3000);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_a_step_back_inside_50_restarts_the_2_s() {
  GameMode g = playing();
  g.onPerson(at(25), 200);
  g.onPerson(at(55), 1000);
  g.onPerson(at(48), 2000);  // back inside 50 cm
  for (uint32_t t = 2100; t <= 3500; t += 200) g.onPerson(at(55), t);  // estimates keep coming (else: held)
  g.step(3500);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  for (uint32_t t = 3700; t <= 4100; t += 200) g.onPerson(at(55), t);
  g.step(4100);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kPlaying), static_cast<int>(g.state()));
}

void test_stall_or_pushing_not_moving_is_caught() {
  GameMode g = playing();
  g.onPerson(at(120), 150);
  g.onCaughtEvent(200);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.justCaught());
}

void test_stale_300_ms_holds_1_s_is_held() {
  GameMode g = playing();
  g.onPerson(at(200, 0), 0);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 299)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 301)));
  g.step(1001);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  g.onPerson(at(200), 1100);  // a person again does not leave Held
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  g.onRunEnd();  // nor the run ending (S12: only a person's STOP)
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(g.state()));
  bool on = true;
  while (g.wantsEnvelope(on)) {
  }
  g.onStop();
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kOff), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_FALSE(on);  // the envelope cleared
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
  c.onPerson(at(20), 150);
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
  TEST_ASSERT_TRUE(g.onUnoHello());
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kArming), static_cast<int>(g.state()));
  TEST_ASSERT_TRUE(g.wantsEnvelope(on));
  TEST_ASSERT_TRUE(on);
  g.onPerson(at(200), 150);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(g.gate(kForward, 20, 160)));
  g.onEnvelopeAck(170);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 180)));
  GameMode off;
  TEST_ASSERT_FALSE(off.onUnoHello());
}

void test_review_focus_4_person_then_gate_same_tick() {
  GameMode g = playing();
  g.onPerson(at(80), 150);
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 150)));
  g.onPerson(at(25), 160);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(g.gate(kForward, 20, 160)));
}

void test_stop_while_arming() {
  GameMode g;
  g.startRun(true, true, 75, 0);
  bool on = false;
  while (g.wantsEnvelope(on)) {
  }
  g.onStop();
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

// ---- KeepOut

void test_forward_towards_ends_kept_away() {
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(60), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kEndKeptAway), static_cast<int>(KeepOut::check(at(55), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kEndKeptAway),
                    static_cast<int>(KeepOut::check(at(50, 59), kForward, 20, 0)));  // within ±60° of ahead
}

void test_forward_away_or_aside_allowed() {
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, 90), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, -61), kForward, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(40, 180), kForward, 20, 0)));
}

void test_turns_from_35_cm() {
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kAllow), static_cast<int>(KeepOut::check(at(36), kTurn, 20, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Gate::kHoldZero), static_cast<int>(KeepOut::check(at(35), kTurn, 20, 0)));
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_non_game_run_never_arms);
  RUN_TEST(test_no_cap_or_no_people_refuses);
  RUN_TEST(test_arming_holds_until_the_envelope_ack);
  RUN_TEST(test_no_ack_within_500_ms_ends_off_as_no_cap);
  RUN_TEST(test_caught_at_30_and_released_after_50_for_2_s);
  RUN_TEST(test_a_step_back_inside_50_restarts_the_2_s);
  RUN_TEST(test_stall_or_pushing_not_moving_is_caught);
  RUN_TEST(test_stale_300_ms_holds_1_s_is_held);
  RUN_TEST(test_no_person_at_all_after_arming_is_held_after_1_s);
  RUN_TEST(test_engine_gone_is_held);
  RUN_TEST(test_uno_hello_rearms);
  RUN_TEST(test_review_focus_4_person_then_gate_same_tick);
  RUN_TEST(test_stop_while_arming);
  RUN_TEST(test_the_run_ending_clears_the_envelope);
  RUN_TEST(test_forward_towards_ends_kept_away);
  RUN_TEST(test_forward_away_or_aside_allowed);
  RUN_TEST(test_turns_from_35_cm);
  RUN_TEST(test_unseen_1_s_holds_forward_and_turns);
  RUN_TEST(test_backward_is_never_cut_by_bearing);
  RUN_TEST(test_standing_still_is_always_allowed);
  return UNITY_END();
}
