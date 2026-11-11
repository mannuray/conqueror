// GameController (bridge LLD R34): game mode joined to the bridge - the run's start and end, the envelope, the
// engine's person estimates, caught, held, back_ok and the gate every DRIVE asks. Hooks are recorded by fakes.
// Final review fixes: the envelope's ack tied to its own op (C2), estimates at receipt (I6), long holds end the move
// (I4), the game cap on every DRIVE and a stopping distance from the speed (I7), Held refuses a run (I2).
#include <unity.h>

#include "app/game_controller.h"

using cq::app::GameController;
using cq::core::GameState;
using cq::core::PersonEst;
namespace proto = cq::proto;

namespace {
struct Rec {
  bool people = true;
  int game_on = -1;  // last {"t":"game","on"} sent: 1 / 0
  int env_n = 0;
  uint8_t env_cap = 0, env_lock = 0;
  bool queue_full = false;
  int keep_away = 0, caught = 0;
  uint16_t ended = 0;
  int asks = 0;
  int16_t ask_cm = 0;
  GameState last_event = GameState::kOff;
  int16_t event_d = -2;
  int events = 0;
  uint16_t sonar_mm = 65535;
};
Rec rec;

GameController::Hooks hooks() {
  GameController::Hooks h;
  h.people = [] { return rec.people; };
  h.send_game = [](bool on) { rec.game_on = on ? 1 : 0; };
  h.queue_envelope = [](uint8_t cap, uint8_t, uint8_t lock) {
    if (rec.queue_full) return false;
    ++rec.env_n;
    rec.env_cap = cap;
    rec.env_lock = lock;
    return true;
  };
  h.keep_away = [](uint32_t) { ++rec.keep_away; };
  h.caught = [](uint32_t) { ++rec.caught; };
  h.end_run = [](uint16_t code, uint32_t) { rec.ended = code; };
  h.ask_back = [](int16_t cm) {
    ++rec.asks;
    rec.ask_cm = cm;
    return static_cast<int32_t>(40 + rec.asks);
  };
  h.event = [](GameState s, int16_t d, uint16_t) {
    rec.last_event = s;
    rec.event_d = d;
    ++rec.events;
  };
  h.roll_cm = [] { return static_cast<uint16_t>(3); };
  h.sonar_mm = [] { return rec.sonar_mm; };
  return h;
}

void see(GameController& g, uint32_t now, int16_t d, int16_t bearing = 0, bool seen = true) {
  g.onPerson(PersonEst{seen, d, bearing, 0, 200, now}, now);
}

// A game armed and playing at t = 20 ms, a person at 120 cm straight ahead.
GameController playing() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  TEST_ASSERT_EQUAL_UINT16(0, g.onRunStart(true, 0));
  g.tick(1);
  g.onEnvelopeDone(0, true, 10);
  see(g, 20, 120);
  g.tick(20);
  return g;
}

bool drive(GameController& g, int8_t v, int8_t w, proto::Source src = proto::Source::kAuto) {
  return GameController::gate(&g, v, w, src);
}
}  // namespace

void setUp() { rec = Rec(); }
void tearDown() {}

void test_refusals() {
  GameController g;
  g.setHooks(hooks());
  TEST_ASSERT_EQUAL_UINT16(7060, g.onRunStart(true, 0));  // no cap measured
  g.setCap(75);
  rec.people = false;
  TEST_ASSERT_EQUAL_UINT16(7061, g.onRunStart(true, 0));  // no seeing test
  TEST_ASSERT_EQUAL_UINT16(0, g.onRunStart(false, 0));    // not a game: nothing to check
  g.tick(5);
  TEST_ASSERT_EQUAL(0, rec.env_n);
  TEST_ASSERT_EQUAL(-1, rec.game_on);
}

void test_a_game_arms_the_envelope_and_waits_for_its_ack() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(true, 0);
  TEST_ASSERT_FALSE(g.ready());
  g.tick(1);
  TEST_ASSERT_EQUAL(1, rec.env_n);
  TEST_ASSERT_EQUAL_UINT8(75, rec.env_cap);
  TEST_ASSERT_EQUAL_UINT8(2, rec.env_lock);  // CHILD_PLAY
  TEST_ASSERT_EQUAL(1, rec.game_on);         // the engine starts its person stream
  g.onEnvelopeDone(0, true, 10);
  TEST_ASSERT_TRUE(g.ready());
}

void test_the_ack_of_a_clearing_envelope_never_arms() {  // C2
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onBodyOnline(0);  // the boot clear is queued ...
  g.tick(1);
  g.onRunStart(true, 2);  // ... and a game arms behind it
  g.tick(3);
  TEST_ASSERT_EQUAL(2, rec.env_n);
  g.onEnvelopeDone(0, false, 10);  // the clear's ack
  TEST_ASSERT_FALSE(g.ready());
  g.onEnvelopeDone(0, true, 20);  // the game's own
  TEST_ASSERT_TRUE(g.ready());
}

void test_an_older_game_envelope_ack_never_arms_a_later_one() {  // C2: on, off, on - only the last on arms
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(true, 0);
  g.tick(1);
  g.onRunEnd(2);  // ended before its ack: a clear queued
  g.onRunStart(true, 3);
  g.tick(4);
  TEST_ASSERT_EQUAL(3, rec.env_n);
  g.onEnvelopeDone(0, true, 10);  // the first game's on
  TEST_ASSERT_FALSE(g.ready());
  g.onEnvelopeDone(0, false, 11);  // the clear
  TEST_ASSERT_FALSE(g.ready());
  g.onEnvelopeDone(0, true, 12);  // this game's on
  TEST_ASSERT_TRUE(g.ready());
}

void test_a_refused_game_envelope_ends_the_run_7060() {  // C2
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(true, 0);
  g.tick(1);
  g.onEnvelopeDone(3, true, 10);
  g.tick(11);
  TEST_ASSERT_EQUAL_UINT16(7060, rec.ended);
}

void test_a_failed_clear_is_sent_again() {  // final review minor
  GameController g = playing();
  g.onRunEnd(30);
  TEST_ASSERT_EQUAL(2, rec.env_n);
  g.onEnvelopeDone(3, false, 40);
  g.tick(41);
  TEST_ASSERT_EQUAL(3, rec.env_n);
  TEST_ASSERT_EQUAL_UINT8(100, rec.env_cap);
}

void test_a_full_queue_retries_the_envelope() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(true, 0);
  rec.queue_full = true;
  g.tick(1);
  TEST_ASSERT_EQUAL(0, rec.env_n);
  rec.queue_full = false;
  g.tick(3);
  TEST_ASSERT_EQUAL(1, rec.env_n);
}

void test_no_ack_ends_the_run_7060() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(true, 0);
  g.tick(1);
  g.tick(cq::core::GameMode::kArmAckMs + 10);
  TEST_ASSERT_EQUAL_UINT16(7060, rec.ended);
}

void test_caught_ends_the_motion_once_and_tells_the_pages() {
  GameController g = playing();
  see(g, 40, 28);
  g.tick(40);
  TEST_ASSERT_EQUAL(1, rec.caught);
  TEST_ASSERT_TRUE(g.caught());
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(rec.last_event));
  TEST_ASSERT_EQUAL_INT16(28, rec.event_d);
  see(g, 60, 28);
  g.tick(60);
  TEST_ASSERT_EQUAL(1, rec.caught);  // once
}

void test_an_estimate_counts_at_once() {  // I6: a DRIVE right after an estimate is gated against it
  GameController g = playing();
  TEST_ASSERT_TRUE(drive(g, 30, 0));
  see(g, 30, 25);  // before the next tick
  TEST_ASSERT_FALSE(drive(g, 30, 0));
}

void test_the_engine_gone_holds_and_ends_the_run_7052() {
  GameController g = playing();
  g.onEngineGone(30);
  g.tick(31);
  TEST_ASSERT_EQUAL_UINT16(7052, rec.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(rec.last_event));
  TEST_ASSERT_FALSE(drive(g, 30, 0, proto::Source::kHuman));  // the stick too: zeros
  g.onRunEnd(40);  // the run ending does not release Held
  TEST_ASSERT_FALSE(drive(g, 30, 0));
  TEST_ASSERT_EQUAL_UINT16(7052, g.onRunStart(false, 45));  // nor a new run (I2)
  g.onStop(50, true);
  g.tick(51);
  TEST_ASSERT_TRUE(drive(g, 30, 0));
  TEST_ASSERT_EQUAL(0, rec.game_on);
  TEST_ASSERT_EQUAL_UINT8(100, rec.env_cap);  // the envelope cleared
  TEST_ASSERT_EQUAL_UINT8(0, rec.env_lock);
}

// Stopping distance (review I7): roll 3 cm + speed x (0.3 s + the estimate's age); speed = v % x 33.5 cm/s / 100.
void test_forward_close_ahead_ends_kept_away() {
  GameController g = playing();
  see(g, 30, 50);
  TEST_ASSERT_TRUE(drive(g, 30, 0));  // 10 cm/s: stop 3 + 3 = 6; 50 - 6 - 5 = 39 > 30
  g.tick(32);
  TEST_ASSERT_EQUAL(0, rec.keep_away);
  see(g, 40, 40);
  TEST_ASSERT_FALSE(drive(g, 30, 0));  // 40 - 6 - 5 = 29: towards - zeros, and the move ends normally
  g.tick(42);
  TEST_ASSERT_EQUAL(1, rec.keep_away);
}

void test_a_long_hold_ends_the_move() {  // I4: the motion's no-progress watch must never see a long hold
  GameController g = playing();
  for (uint32_t t = 40; t <= 340; t += 20) {
    see(g, t, 120, 0, false);  // out of sight
    g.tick(t);
    drive(g, 30, 0);
  }
  TEST_ASSERT_EQUAL(0, rec.keep_away);  // unseen under 1 s: still allowed
  for (uint32_t t = 360; t <= 1400; t += 20) {
    see(g, t, 120, 0, false);
    g.tick(t);
    drive(g, 30, 0);
  }
  TEST_ASSERT_TRUE(rec.keep_away >= 1);  // held from ~1020 ms, ended by ~1320 ms
}

void test_the_game_cap_clamps_every_drive() {  // I7
  GameController g = playing();
  int8_t v = 100, w = 20;
  TEST_ASSERT_TRUE(GameController::gateClamp(&g, v, w, proto::Source::kHuman));
  TEST_ASSERT_EQUAL_INT8(75, v);
  TEST_ASSERT_EQUAL_INT8(15, w);
  GameController off;
  off.setHooks(hooks());
  int8_t v2 = 100, w2 = 20;
  TEST_ASSERT_TRUE(GameController::gateClamp(&off, v2, w2, proto::Source::kHuman));
  TEST_ASSERT_EQUAL_INT8(100, v2);  // no game: untouched
}

void test_the_stopping_distance_grows_with_speed_and_age() {  // I7
  GameController g = playing();
  see(g, 30, 45);
  TEST_ASSERT_TRUE(drive(g, 20, 0));   // 6.7 cm/s: stop 3 + 2 = 5; 45 - 5 - 5 = 35 > 30
  TEST_ASSERT_FALSE(drive(g, 75, 0));  // 25 cm/s: stop 3 + 7.5 = 10; 45 - 10 - 5 = 30, not > 30
  GameController h = playing();
  h.onPerson(PersonEst{true, 50, 0, 0, 200, 40}, 40);
  TEST_ASSERT_TRUE(drive(h, 75, 0));  // fresh: 50 - 10 - 5 = 35
  h.onPerson(PersonEst{true, 50, 0, 0, 200, 40}, 240);  // the same estimate, 200 ms old
  TEST_ASSERT_FALSE(drive(h, 75, 0));  // stop 3 + 25 x 0.5 = 15: 50 - 15 - 5 = 30
}

void test_backing_asks_the_map_first() {
  GameController g = playing();
  g.backing(25);
  TEST_ASSERT_FALSE(drive(g, -30, 0));  // held while asking
  TEST_ASSERT_EQUAL(1, rec.asks);
  TEST_ASSERT_EQUAL_INT16(25, rec.ask_cm);
  TEST_ASSERT_FALSE(drive(g, -30, 0));
  TEST_ASSERT_EQUAL(1, rec.asks);  // asked once
  g.onAnswer(41, 1, 50);
  TEST_ASSERT_TRUE(drive(g, -30, 0));  // yes: it backs
  TEST_ASSERT_TRUE(drive(g, 0, 0));    // the backward move ended
  TEST_ASSERT_FALSE(drive(g, -30, 0, proto::Source::kHuman));  // the stick backing asks again
  TEST_ASSERT_EQUAL(2, rec.asks);
  TEST_ASSERT_EQUAL_INT16(30, rec.ask_cm);  // no length given: 30 cm
}

void test_a_skills_backing_is_not_asked() {  // final review minor: the engine checks its own way out
  GameController g = playing();
  TEST_ASSERT_TRUE(drive(g, -30, 0, proto::Source::kAuto));
  TEST_ASSERT_EQUAL(0, rec.asks);
}

void test_a_yes_lasts_3_s() {  // final review minor: one yes is not a licence to back forever
  GameController g = playing();
  TEST_ASSERT_FALSE(drive(g, -30, 0, proto::Source::kHuman));
  g.onAnswer(41, 1, 30);
  TEST_ASSERT_TRUE(drive(g, -30, 0, proto::Source::kHuman));
  for (uint32_t t = 40; t <= 3100; t += 100) {
    see(g, t, 120);
    g.tick(t);
  }
  TEST_ASSERT_FALSE(drive(g, -30, 0, proto::Source::kHuman));  // asks again
  TEST_ASSERT_EQUAL(2, rec.asks);
}

void test_backing_refused_or_unanswered_is_kept_away() {
  GameController g = playing();
  g.backing(20);
  TEST_ASSERT_FALSE(drive(g, -30, 0));
  g.onAnswer(41, 0, 50);  // no
  TEST_ASSERT_FALSE(drive(g, -30, 0));
  g.tick(52);
  TEST_ASSERT_EQUAL(1, rec.keep_away);
  GameController h = playing();
  rec.keep_away = 0;
  h.backing(20);
  TEST_ASSERT_FALSE(drive(h, -30, 0));
  see(h, 390, 120);
  h.tick(400);  // no answer within 300 ms
  TEST_ASSERT_FALSE(drive(h, -30, 0));
  h.tick(402);
  TEST_ASSERT_EQUAL(1, rec.keep_away);
}

void test_an_old_answer_is_ignored() {
  GameController g = playing();
  g.backing(20);
  TEST_ASSERT_FALSE(drive(g, -30, 0));
  g.onAnswer(7, 1, 50);  // not this question's id
  TEST_ASSERT_FALSE(drive(g, -30, 0));
}

void test_body_online_rearms_a_game_or_clears_when_off() {
  GameController off;
  off.setHooks(hooks());
  off.onBodyOnline(5);
  off.tick(6);
  TEST_ASSERT_EQUAL(1, rec.env_n);  // the boot / reboot clear
  TEST_ASSERT_EQUAL_UINT8(100, rec.env_cap);
  rec = Rec();
  GameController g = playing();
  rec.env_n = 0;
  g.onBodyOnline(4000);  // C1: a restart long after the start gets a fresh arming window
  TEST_ASSERT_FALSE(g.ready());
  g.tick(4001);
  TEST_ASSERT_EQUAL(1, rec.env_n);
  TEST_ASSERT_EQUAL_UINT8(75, rec.env_cap);
  g.tick(4600);
  TEST_ASSERT_EQUAL_UINT16(0, rec.ended);
  g.onEnvelopeDone(0, true, 4700);
  TEST_ASSERT_TRUE(g.ready());
}

void test_a_non_game_never_gates() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(false, 0);
  g.tick(1);
  TEST_ASSERT_TRUE(drive(g, 30, 0));
  TEST_ASSERT_TRUE(drive(g, -30, 0, proto::Source::kHuman));
  TEST_ASSERT_EQUAL(0, rec.asks);
}

void test_the_sonar_ahead_catches_when_the_camera_reads_far() {  // R34.11, design-notes §235
  GameController g = playing();
  rec.sonar_mm = 480;  // the body at 48 cm: the hands ~28 cm
  see(g, 30, 60);      // the camera boxed only the body
  g.tick(30);
  TEST_ASSERT_TRUE(g.caught());
  TEST_ASSERT_EQUAL_INT(1, rec.caught);
}

void test_the_sonar_aside_does_not_catch() {
  GameController g = playing();
  rec.sonar_mm = 480;  // a chair ahead, the person 40 deg aside
  see(g, 30, 60, 40);
  g.tick(30);
  TEST_ASSERT_FALSE(g.caught());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusals);
  RUN_TEST(test_a_game_arms_the_envelope_and_waits_for_its_ack);
  RUN_TEST(test_the_ack_of_a_clearing_envelope_never_arms);
  RUN_TEST(test_an_older_game_envelope_ack_never_arms_a_later_one);
  RUN_TEST(test_a_refused_game_envelope_ends_the_run_7060);
  RUN_TEST(test_a_failed_clear_is_sent_again);
  RUN_TEST(test_a_full_queue_retries_the_envelope);
  RUN_TEST(test_no_ack_ends_the_run_7060);
  RUN_TEST(test_caught_ends_the_motion_once_and_tells_the_pages);
  RUN_TEST(test_an_estimate_counts_at_once);
  RUN_TEST(test_the_engine_gone_holds_and_ends_the_run_7052);
  RUN_TEST(test_forward_close_ahead_ends_kept_away);
  RUN_TEST(test_a_long_hold_ends_the_move);
  RUN_TEST(test_the_game_cap_clamps_every_drive);
  RUN_TEST(test_the_stopping_distance_grows_with_speed_and_age);
  RUN_TEST(test_backing_asks_the_map_first);
  RUN_TEST(test_a_skills_backing_is_not_asked);
  RUN_TEST(test_a_yes_lasts_3_s);
  RUN_TEST(test_backing_refused_or_unanswered_is_kept_away);
  RUN_TEST(test_an_old_answer_is_ignored);
  RUN_TEST(test_body_online_rearms_a_game_or_clears_when_off);
  RUN_TEST(test_a_non_game_never_gates);
  RUN_TEST(test_the_sonar_ahead_catches_when_the_camera_reads_far);
  RUN_TEST(test_the_sonar_aside_does_not_catch);
  return UNITY_END();
}
