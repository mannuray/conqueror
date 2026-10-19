// Plan N2 Task 5: effects → actions - units and signs of the motion service, run-time ranges and types, colours,
// sounds, LOOK, ARC speed band and 7022, notes, and the running.yaml text of each action (LLD §10).
#include <string.h>
#include <unity.h>

#include <initializer_list>

#include "core/arc_limits.h"
#include "logo/effects.h"

using namespace cq::logo;

namespace {
Program g_p;
Action g_a;
RunError g_err;
MapState g_st;

Value num(int32_t milli) { return Value{VTag::kNum, milli}; }
Value word(const char* s) { return Value{VTag::kWord, g_p.addWord(s, static_cast<uint16_t>(strlen(s)))}; }
Value list(std::initializer_list<int32_t> milli) {
  Value items[8];
  uint16_t n = 0;
  for (int32_t m : milli) items[n++] = num(m);
  return Value{VTag::kList, g_p.addList(items, n)};
}
bool map(EffectKind k, std::initializer_list<Value> args) {
  Effect e;
  e.kind = k;
  e.line = 7;
  for (const Value& v : args) e.args[e.n++] = v;
  g_err = RunError();
  return mapEffect(e, g_p, g_st, g_a, g_err);
}
const char* text() {
  static char out[160];
  actionText(g_a, out, sizeof out);
  return out;
}
}  // namespace

void setUp() {
  g_p.clear();
  g_st = MapState();
}
void tearDown() {}

void test_moves_round_to_whole_cm_and_back_is_negative() {
  TEST_ASSERT_TRUE(map(EffectKind::kForward, {num(50000)}));
  TEST_ASSERT_EQUAL(ActKind::kMove, g_a.kind);
  TEST_ASSERT_EQUAL(50, g_a.dist_cm);
  TEST_ASSERT_EQUAL(40, g_a.speed_pct);
  TEST_ASSERT_EQUAL_STRING("FORWARD 50", text());
  TEST_ASSERT_TRUE(map(EffectKind::kBack, {num(2500)}));
  TEST_ASSERT_EQUAL(-3, g_a.dist_cm);
  TEST_ASSERT_EQUAL_STRING("BACK 3", text());
}

void test_run_time_range_and_type_errors_have_the_line() {
  TEST_ASSERT_FALSE(map(EffectKind::kForward, {num(600000)}));
  TEST_ASSERT_EQUAL(7025, g_err.code);
  TEST_ASSERT_EQUAL(7, g_err.line);
  TEST_ASSERT_EQUAL_STRING("FORWARD needs a distance from 1 to 500 cm.", g_err.msg);
  TEST_ASSERT_FALSE(map(EffectKind::kForward, {num(0)}));
  TEST_ASSERT_EQUAL(7025, g_err.code);
  TEST_ASSERT_FALSE(map(EffectKind::kRight, {word("cat")}));
  TEST_ASSERT_EQUAL_STRING("RIGHT needs a number, but got cat.", g_err.msg);
}

void test_turns_right_is_minus_left_is_plus() {
  TEST_ASSERT_TRUE(map(EffectKind::kRight, {num(90000)}));
  TEST_ASSERT_EQUAL(-9000, g_a.delta_cdeg);
  TEST_ASSERT_EQUAL_STRING("RIGHT 90", text());
  TEST_ASSERT_TRUE(map(EffectKind::kLeft, {num(45500)}));
  TEST_ASSERT_EQUAL(4550, g_a.delta_cdeg);
  TEST_ASSERT_EQUAL_STRING("LEFT 45.5", text());
  TEST_ASSERT_TRUE(map(EffectKind::kPivot, {word("left"), num(90000)}));
  TEST_ASSERT_EQUAL(ActKind::kPivot, g_a.kind);
  TEST_ASSERT_EQUAL(9000, g_a.delta_cdeg);
  TEST_ASSERT_EQUAL_STRING("PIVOT LEFT 90", text());
}

void test_arc_speed_band_and_too_tight() {
  const cq::core::ArcBand band = cq::core::arcBand(50);
  g_st.speed_pct = 100;
  TEST_ASSERT_TRUE(map(EffectKind::kArc, {word("RIGHT"), num(90000), num(50000)}));
  TEST_ASSERT_EQUAL(-9000, g_a.delta_cdeg);
  TEST_ASSERT_EQUAL(50, g_a.radius_cm);
  TEST_ASSERT_EQUAL(band.hi, g_a.speed_pct);
  TEST_ASSERT_EQUAL(band.hi != 100, g_a.speed_moved);
  TEST_ASSERT_EQUAL_STRING("ARC RIGHT 90 50", text());
  TEST_ASSERT_FALSE(map(EffectKind::kArc, {word("LEFT"), num(90000), num(20000)}));
  TEST_ASSERT_EQUAL(7022, g_err.code);
  TEST_ASSERT_EQUAL_STRING("ARC LEFT 90 20 is too tight — the smallest circle is 31 cm. Try PIVOT.", g_err.msg);
  TEST_ASSERT_FALSE(map(EffectKind::kArc, {word("UP"), num(90000), num(50000)}));
  TEST_ASSERT_EQUAL(7029, g_err.code);
}

void test_led_colours() {
  TEST_ASSERT_TRUE(map(EffectKind::kLed, {word("green")}));
  TEST_ASSERT_EQUAL_STRING("LED 0 255 0", text());
  TEST_ASSERT_TRUE(map(EffectKind::kLed, {list({255000, 128000, 0})}));
  TEST_ASSERT_EQUAL_STRING("LED 255 128 0", text());
  TEST_ASSERT_FALSE(map(EffectKind::kLed, {list({1000, 2000})}));
  TEST_ASSERT_EQUAL_STRING("LED needs a colour, but got [1 2].", g_err.msg);
  TEST_ASSERT_FALSE(map(EffectKind::kLed, {list({300000, 0, 0})}));
  TEST_ASSERT_EQUAL_STRING("LED needs a colour part from 0 to 255.", g_err.msg);
  TEST_ASSERT_FALSE(map(EffectKind::kLed, {word("PINK")}));
  TEST_ASSERT_EQUAL(7029, g_err.code);
  TEST_ASSERT_NOT_NULL(strstr(g_err.msg, "PINK isn't one of them"));
}

void test_sound_look_beep_home() {
  TEST_ASSERT_TRUE(map(EffectKind::kSound, {word("horn")}));
  TEST_ASSERT_EQUAL(24, g_a.sound_id);
  TEST_ASSERT_EQUAL_STRING("SOUND HORN", text());
  TEST_ASSERT_TRUE(map(EffectKind::kLook, {word("LEFT")}));
  TEST_ASSERT_EQUAL_STRING("LOOK -60 0", text());
  TEST_ASSERT_TRUE(map(EffectKind::kLook, {word("HOME")}));
  TEST_ASSERT_EQUAL(ActKind::kLookHome, g_a.kind);
  TEST_ASSERT_TRUE(map(EffectKind::kLook, {list({30000, -10000})}));
  TEST_ASSERT_EQUAL_STRING("LOOK 30 -10", text());
  TEST_ASSERT_TRUE(map(EffectKind::kBeep, {}));
  TEST_ASSERT_EQUAL_STRING("BEEP", text());
  TEST_ASSERT_TRUE(map(EffectKind::kHome, {}));
  TEST_ASSERT_EQUAL_STRING("HOME", text());
}

void test_play_wait_say_speed() {
  const Value items[] = {word("C4"), num(2000), word("R")};
  TEST_ASSERT_TRUE(map(EffectKind::kPlay, {Value{VTag::kList, g_p.addList(items, 3)}}));
  TEST_ASSERT_EQUAL(ActKind::kTone, g_a.kind);
  TEST_ASSERT_EQUAL(750, g_a.ms);
  TEST_ASSERT_EQUAL_STRING("PLAY 262/500 0/250", text());
  TEST_ASSERT_TRUE(map(EffectKind::kWait, {num(1500)}));
  TEST_ASSERT_EQUAL(1500, g_a.ms);
  TEST_ASSERT_EQUAL_STRING("WAIT 1.5", text());
  const Value say[] = {word("well"), word("done")};
  TEST_ASSERT_TRUE(map(EffectKind::kSay, {Value{VTag::kList, g_p.addList(say, 2)}}));
  TEST_ASSERT_EQUAL_STRING("SAY well done", text());
  TEST_ASSERT_TRUE(map(EffectKind::kSpeed, {num(60000)}));
  TEST_ASSERT_EQUAL_STRING("SPEED 60", text());
  TEST_ASSERT_FALSE(map(EffectKind::kSpeed, {num(5000)}));
  TEST_ASSERT_EQUAL(7025, g_err.code);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_moves_round_to_whole_cm_and_back_is_negative);
  RUN_TEST(test_run_time_range_and_type_errors_have_the_line);
  RUN_TEST(test_turns_right_is_minus_left_is_plus);
  RUN_TEST(test_arc_speed_band_and_too_tight);
  RUN_TEST(test_led_colours);
  RUN_TEST(test_sound_look_beep_home);
  RUN_TEST(test_play_wait_say_speed);
  return UNITY_END();
}
