// Plan N2 Task 6: the RunController with a fake motion layer - every ending of LLD §9, BLOCKED?, waits, the time
// limit, step mode, HOME, sensors from telemetry, and a second ending ignored.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "logo/checker.h"
#include "logo/compiler.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/run_controller.h"

using namespace cq::logo;
using cq::core::EndReason;
using cq::core::MotionEvent;
using cq::core::PrimitiveEnd;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
Program g_prog;
Vm g_vm;
RunController g_rc(g_vm);
RunTelem g_tel;
uint32_t g_now;
char g_trace[2048];
RunOut g_last;
// The fake motion layer: how the n-th motion (1-based) ends; kDone for the others.
uint8_t g_motion;
uint8_t g_odd_motion;
EndReason g_odd_reason;

bool load(const char* src, bool step = false) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_d.clear();
  pool.reset();
  lex(src, static_cast<uint16_t>(strlen(src)), g_t, g_d);
  Node* prog = p.parse(src, g_t);
  const CheckResult r = Checker(p, src, g_t, g_d).check(prog, Caps());
  if (!r.ok || !Compiler(p, src, g_t, g_d).compile(prog, r.handles_obstacles, g_prog)) return false;
  g_tel = RunTelem();
  g_now = 1000;
  g_trace[0] = '\0';
  g_motion = 0;
  g_odd_motion = 0;
  RunConfig cfg;
  cfg.step = step;
  g_rc.start(g_prog, cfg, g_tel, g_now);
  return true;
}

void note(const RunOut& o) {
  if (!o.act || o.action.kind == ActKind::kUntilStart) return;
  char one[160];
  actionText(o.action, one, sizeof one);
  if (g_trace[0] != '\0') strncat(g_trace, " | ", sizeof g_trace - strlen(g_trace) - 1);
  strncat(g_trace, one, sizeof g_trace - strlen(g_trace) - 1);
}

// A motion the fake layer finishes at once: turns move the yaw, the n-th motion may end oddly.
RunOut finishMotion(const Action& a) {
  if (a.kind == ActKind::kTurn || a.kind == ActKind::kHome || a.kind == ActKind::kPivot || a.kind == ActKind::kArc) {
    g_tel.yaw_cdeg = static_cast<uint16_t>((g_tel.yaw_cdeg + 36000 + a.delta_cdeg % 36000) % 36000);
  }
  PrimitiveEnd e;
  e.reason = ++g_motion == g_odd_motion ? g_odd_reason : EndReason::kDone;
  e.ok = e.reason == EndReason::kDone;
  return g_rc.onPrimitiveEnd(e);
}

// Ticks every 2 ms (jumping over waits) until the program ends or `max_ticks` pass.
void drive(int max_ticks = 400000) {
  for (int i = 0; i < max_ticks; ++i) {
    if (g_rc.state() == RunState::kWaiting && g_rc.wakeAt() > g_now) g_now = g_rc.wakeAt();
    g_last = g_rc.tick(g_tel, g_now);
    note(g_last);
    if (g_rc.state() == RunState::kMotion) g_last = finishMotion(g_last.action);
    if (g_last.ended || g_rc.state() == RunState::kEnded || g_rc.state() == RunState::kPaused) return;
    g_now += 2;
  }
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_square_finishes() {
  TEST_ASSERT_TRUE(load("REPEAT 2 [ FD 50 RT 90 ]"));
  drive();
  TEST_ASSERT_EQUAL_STRING("FORWARD 50 | RIGHT 90 | FORWARD 50 | RIGHT 90", g_trace);
  TEST_ASSERT_TRUE(g_last.ended);
  TEST_ASSERT_EQUAL(0, g_last.code);
  TEST_ASSERT_EQUAL(RunState::kEnded, g_rc.state());
}

void test_stop_word_is_7040() {
  TEST_ASSERT_TRUE(load("FD 10\nSTOP\nFD 20"));
  drive();
  TEST_ASSERT_EQUAL(7040, g_last.code);
  TEST_ASSERT_EQUAL(2, g_last.end_line);
  TEST_ASSERT_EQUAL_STRING("Stopped by you.", g_last.msg);
}

void test_cut_short_without_blocked_ends_7024() {
  TEST_ASSERT_TRUE(load("FD 10\nFD 100\nFD 20"));
  g_odd_motion = 2;
  g_odd_reason = EndReason::kBlocked;
  drive();
  TEST_ASSERT_EQUAL_STRING("FORWARD 10 | FORWARD 100", g_trace);
  TEST_ASSERT_EQUAL(7024, g_last.code);
  TEST_ASSERT_EQUAL_STRING("Something is in the way on line 2.", g_last.msg);
}

void test_cut_short_with_blocked_goes_on() {
  TEST_ASSERT_TRUE(load("FD 100 IF BLOCKED? [ SAY [ oops ] BK 10 ] IF BLOCKED? [ BEEP ] FD 20"));
  g_odd_motion = 1;
  g_odd_reason = EndReason::kBlocked;
  drive();
  TEST_ASSERT_EQUAL_STRING("FORWARD 100 | SAY oops | BACK 10 | FORWARD 20", g_trace);  // BK 10 ended normally
  TEST_ASSERT_EQUAL(0, g_last.code);
}

void test_end_reasons_of_a_motion() {
  const EndReason reasons[] = {EndReason::kStalled, EndReason::kEncoderFault, EndReason::kNotOwner, EndReason::kStop,
                               EndReason::kLink};
  const uint16_t codes[] = {7044, 7021, 7045, 7040, 7034};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(load("FD 10 FD 20"));
    g_odd_motion = 1;
    g_odd_reason = reasons[i];
    drive();
    TEST_ASSERT_EQUAL(codes[i], g_last.code);
  }
  TEST_ASSERT_TRUE(load("FD 10"));
  g_odd_motion = 1;
  g_odd_reason = EndReason::kEncoderFault;
  drive();
  TEST_ASSERT_EQUAL_STRING("This tank can't measure distance yet (FORWARD needs encoders).", g_last.msg);
}

void test_events_while_waiting_and_a_second_ending_is_ignored() {
  TEST_ASSERT_TRUE(load("WAIT 10 FD 1"));
  g_last = g_rc.tick(g_tel, g_now);
  TEST_ASSERT_EQUAL(RunState::kWaiting, g_rc.state());
  TEST_ASSERT_EQUAL(g_now + 10000, g_rc.wakeAt());
  const RunOut gone = g_rc.onMotionEvent(MotionEvent::kOwnerClosed);
  TEST_ASSERT_TRUE(gone.ended);
  TEST_ASSERT_EQUAL(7046, gone.code);
  TEST_ASSERT_FALSE(g_rc.onMotionEvent(MotionEvent::kStop).ended);
  TEST_ASSERT_FALSE(g_rc.onEvent(RunEvent::kLogoStop).ended);
  TEST_ASSERT_FALSE(g_rc.tick(g_tel, g_now + 20000).act);
}

void test_page_stop_battery_tilt_and_release() {
  const RunEvent ev[] = {RunEvent::kLogoStop, RunEvent::kBatteryLow, RunEvent::kTilt};
  const uint16_t codes[] = {7040, 7042, 7043};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(load("FOREVER [ WAIT 1 ]"));
    TEST_ASSERT_EQUAL(codes[i], g_rc.onEvent(ev[i]).code);
  }
  TEST_ASSERT_TRUE(load("FOREVER [ WAIT 1 ]"));
  TEST_ASSERT_EQUAL(7045, g_rc.onMotionEvent(MotionEvent::kRelease).code);
}

void test_time_limit_is_7041() {
  TEST_ASSERT_TRUE(load("FOREVER [ WAIT 60 ]"));
  drive();
  TEST_ASSERT_EQUAL(7041, g_last.code);
  TEST_ASSERT_TRUE(g_now - 1000 >= 300000);
}

void test_empty_forever_never_hangs_a_tick() {
  TEST_ASSERT_TRUE(load("FOREVER [ ]"));
  for (int i = 0; i < 10; ++i) TEST_ASSERT_FALSE(g_rc.tick(g_tel, g_now += 2).ended);
  TEST_ASSERT_EQUAL(RunState::kRunning, g_rc.state());
}

void test_step_mode() {
  TEST_ASSERT_TRUE(load("FD 1\nRT 2", true));
  g_last = g_rc.tick(g_tel, g_now);
  TEST_ASSERT_TRUE(g_last.line);
  TEST_ASSERT_EQUAL(1, g_last.line_no);
  TEST_ASSERT_EQUAL(RunState::kPaused, g_rc.state());
  TEST_ASSERT_FALSE(g_rc.tick(g_tel, g_now += 2).act);  // stays paused
  g_rc.onEvent(RunEvent::kStep);
  drive();
  TEST_ASSERT_EQUAL_STRING("FORWARD 1", g_trace);
  TEST_ASSERT_EQUAL(RunState::kPaused, g_rc.state());
  g_rc.onEvent(RunEvent::kStep);
  drive();
  TEST_ASSERT_EQUAL_STRING("FORWARD 1 | RIGHT 2", g_trace);
}

void test_home_heading_distance_obstacle() {
  g_tel.yaw_cdeg = 0;
  TEST_ASSERT_TRUE(load("RT 90 FD HEADING HOME FD HEADING + 1"));
  drive();
  TEST_ASSERT_EQUAL_STRING("RIGHT 90 | FORWARD 90 | HOME | FORWARD 1", g_trace);
  TEST_ASSERT_TRUE(load("FD DISTANCE IF OBSTACLE? [ BEEP ]"));
  g_tel.distance_mm = 120;
  drive();
  TEST_ASSERT_EQUAL_STRING("FORWARD 12 | BEEP", g_trace);
}

void test_run_time_refusal_ends_with_its_line_and_speed_applies() {
  TEST_ASSERT_TRUE(load("TO F :n FD :n END\nSPEED 60 F 10\nF 600"));
  drive();
  TEST_ASSERT_EQUAL_STRING("SPEED 60 | FORWARD 10", g_trace);
  TEST_ASSERT_EQUAL(7025, g_last.code);
  TEST_ASSERT_EQUAL(1, g_last.end_line);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_square_finishes);
  RUN_TEST(test_stop_word_is_7040);
  RUN_TEST(test_cut_short_without_blocked_ends_7024);
  RUN_TEST(test_cut_short_with_blocked_goes_on);
  RUN_TEST(test_end_reasons_of_a_motion);
  RUN_TEST(test_events_while_waiting_and_a_second_ending_is_ignored);
  RUN_TEST(test_page_stop_battery_tilt_and_release);
  RUN_TEST(test_time_limit_is_7041);
  RUN_TEST(test_empty_forever_never_hangs_a_tick);
  RUN_TEST(test_step_mode);
  RUN_TEST(test_home_heading_distance_obstacle);
  RUN_TEST(test_run_time_refusal_ends_with_its_line_and_speed_applies);
  return UNITY_END();
}
