// GameController (bridge LLD R34): game mode joined to the bridge - the run's start and end, the envelope, the
// engine's person stream, caught, held, back_ok and the gate every DRIVE asks. Hooks are recorded by fakes.
#include <unity.h>

#include "app/game_controller.h"

using cq::app::GameController;
using cq::core::GameState;
using cq::core::PersonEst;
namespace proto = cq::proto;

namespace {
struct Rec {
  bool people = true;
  bool has_person = false;
  PersonEst person;
  int game_on = -1;  // last {"t":"game","on"} sent: 1 / 0
  int env_n = 0;
  uint8_t env_cap = 0, env_lock = 0;
  bool queue_full = false;
  int keep_away = 0, caught = 0;
  uint16_t ended = 0;
  int asks = 0;
  int16_t ask_cm = 0;
  GameState last_event = GameState::kOff;
  int events = 0;
};
Rec rec;

GameController::Hooks hooks() {
  GameController::Hooks h;
  h.people = [] { return rec.people; };
  h.take_person = [](PersonEst& out) {
    if (!rec.has_person) return false;
    out = rec.person;
    rec.has_person = false;
    return true;
  };
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
  h.event = [](GameState s, int16_t, uint16_t) {
    rec.last_event = s;
    ++rec.events;
  };
  h.stop_cm = [] { return static_cast<uint16_t>(10); };
  return h;
}

void person(int16_t d, int16_t bearing = 0, bool seen = true) {
  rec.person = PersonEst{seen, d, bearing, 0, 200, 0};
  rec.has_person = true;
}

// A game armed and playing at t = 20 ms, a person at 120 cm straight ahead.
GameController playing() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  TEST_ASSERT_EQUAL_UINT16(0, g.onRunStart(true, 0));
  g.tick(1);
  g.onEnvelopeDone(0, 10);
  person(120);
  g.tick(20);
  return g;
}

bool drive(GameController& g, int8_t v, int8_t w) { return GameController::gate(&g, v, w, proto::Source::kAuto); }
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
  g.onEnvelopeDone(0, 10);
  TEST_ASSERT_TRUE(g.ready());
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
  g.tick(600);
  TEST_ASSERT_EQUAL_UINT16(7060, rec.ended);
}

void test_caught_ends_the_motion_once_and_tells_the_pages() {
  GameController g = playing();
  person(28);
  g.tick(40);
  TEST_ASSERT_EQUAL(1, rec.caught);
  TEST_ASSERT_TRUE(g.caught());
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kCaught), static_cast<int>(rec.last_event));
  person(28);
  g.tick(60);
  TEST_ASSERT_EQUAL(1, rec.caught);  // once
}

void test_the_engine_gone_holds_and_ends_the_run_7052() {
  GameController g = playing();
  g.onEngineGone(30);
  g.tick(31);
  TEST_ASSERT_EQUAL_UINT16(7052, rec.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(GameState::kHeld), static_cast<int>(rec.last_event));
  TEST_ASSERT_FALSE(drive(g, 30, 0));  // the stick too: zeros
  g.onRunEnd(40);                   // the run ending does not release Held
  TEST_ASSERT_FALSE(drive(g, 30, 0));
  g.onStop(50);
  g.tick(51);
  TEST_ASSERT_TRUE(drive(g, 30, 0));
  TEST_ASSERT_EQUAL(0, rec.game_on);
  TEST_ASSERT_EQUAL_UINT8(100, rec.env_cap);  // the envelope cleared
  TEST_ASSERT_EQUAL_UINT8(0, rec.env_lock);
}

void test_forward_close_ahead_ends_kept_away() {
  GameController g = playing();
  person(44);
  g.tick(30);
  TEST_ASSERT_FALSE(drive(g, 30, 0));  // 44 - 10 - 5 = 29: towards -> zeros, and the move ends normally
  g.tick(32);
  TEST_ASSERT_EQUAL(1, rec.keep_away);
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
  TEST_ASSERT_FALSE(drive(g, -30, 0)); // the next one asks again
  TEST_ASSERT_EQUAL(2, rec.asks);
  TEST_ASSERT_EQUAL_INT16(30, rec.ask_cm);  // no length given: 30 cm
}

void test_backing_refused_or_unanswered_is_kept_away() {
  GameController g = playing();
  TEST_ASSERT_FALSE(drive(g, -30, 0));
  g.onAnswer(41, 0, 50);  // no
  TEST_ASSERT_FALSE(drive(g, -30, 0));
  g.tick(52);
  TEST_ASSERT_EQUAL(1, rec.keep_away);
  GameController h = playing();
  rec.keep_away = 0;
  TEST_ASSERT_FALSE(drive(h, -30, 0));
  h.tick(400);  // no answer within 300 ms
  TEST_ASSERT_FALSE(drive(h, -30, 0));
  h.tick(402);
  TEST_ASSERT_EQUAL(1, rec.keep_away);
}

void test_an_old_answer_is_ignored() {
  GameController g = playing();
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
  g.onBodyOnline(40);
  TEST_ASSERT_FALSE(g.ready());
  g.tick(41);
  TEST_ASSERT_EQUAL(1, rec.env_n);
  TEST_ASSERT_EQUAL_UINT8(75, rec.env_cap);
}

void test_a_non_game_never_gates() {
  GameController g;
  g.setHooks(hooks());
  g.setCap(75);
  g.onRunStart(false, 0);
  g.tick(1);
  TEST_ASSERT_TRUE(drive(g, 30, 0));
  TEST_ASSERT_TRUE(drive(g, -30, 0));
  TEST_ASSERT_EQUAL(0, rec.asks);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusals);
  RUN_TEST(test_a_game_arms_the_envelope_and_waits_for_its_ack);
  RUN_TEST(test_a_full_queue_retries_the_envelope);
  RUN_TEST(test_no_ack_ends_the_run_7060);
  RUN_TEST(test_caught_ends_the_motion_once_and_tells_the_pages);
  RUN_TEST(test_the_engine_gone_holds_and_ends_the_run_7052);
  RUN_TEST(test_forward_close_ahead_ends_kept_away);
  RUN_TEST(test_backing_asks_the_map_first);
  RUN_TEST(test_backing_refused_or_unanswered_is_kept_away);
  RUN_TEST(test_an_old_answer_is_ignored);
  RUN_TEST(test_body_online_rearms_a_game_or_clears_when_off);
  RUN_TEST(test_a_non_game_never_gates);
  return UNITY_END();
}
