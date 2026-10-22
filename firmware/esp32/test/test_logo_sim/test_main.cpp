// Plan N2 Task 8: the planned path - ideal moves, turns, arcs and pivots, uncertain after a sensor read, limits,
// simulated time and run-time errors (LLD §11).
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "logo/checker.h"
#include "logo/compiler.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/simulator.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
Program g_prog;
Vm g_vm;
PathPoint g_pts[2000];
PathBuffer g_path{g_pts, 2000};
SimResult g_r;

void plan(const char* src, uint16_t cap = 2000) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_d.clear();
  pool.reset();
  lex(src, static_cast<uint16_t>(strlen(src)), g_t, g_d);
  Node* prog = p.parse(src, g_t);
  const CheckResult r = Checker(p, src, g_t, g_d).check(prog, Caps());
  TEST_ASSERT_TRUE_MESSAGE(r.ok && Compiler(p, src, g_t, g_d).compile(prog, r.handles_obstacles, g_prog), src);
  g_path.cap = cap;
  g_r = Simulator(g_vm).plan(g_prog, g_path);
}
void near(int32_t x, int32_t y, const PathPoint& p) {
  TEST_ASSERT_INT_WITHIN(1, x, p.x_mm);
  TEST_ASSERT_INT_WITHIN(1, y, p.y_mm);
}
const PathPoint& last() { return g_path.pts[g_path.n - 1]; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_square_corners_within_1_mm() {
  plan("REPEAT 4 [ FD 50 RT 90 ]");
  TEST_ASSERT_FALSE(g_r.cut_short);
  TEST_ASSERT_EQUAL(9, g_path.n);  // the start + 4 moves + 4 turns
  near(0, 500, g_path.pts[1]);
  near(500, 500, g_path.pts[3]);
  near(500, 0, g_path.pts[5]);
  near(0, 0, last());
  TEST_ASSERT_EQUAL(0, last().heading_deg);
  TEST_ASSERT_EQUAL(1, last().line);
}

void test_arc_and_pivot() {
  plan("ARC RIGHT 90 50");
  near(500, 500, last());
  TEST_ASSERT_EQUAL(90, last().heading_deg);
  TEST_ASSERT_EQUAL(10, g_path.n);  // the start + a point every 10°
  plan("PIVOT LEFT 90");
  near(-95, 95, last());
  TEST_ASSERT_EQUAL(-90, last().heading_deg);
}

void test_home_turns_back_to_the_start_heading() {
  plan("RT 135 FD 10 HOME");
  TEST_ASSERT_EQUAL(0, last().heading_deg);
}

void test_uncertain_after_a_sensor_read() {
  plan("FD 10\nIF DISTANCE > 5 [ FD 10 ]");
  TEST_ASSERT_EQUAL(3, g_path.n);
  TEST_ASSERT_FALSE(g_path.pts[1].uncertain);
  TEST_ASSERT_TRUE(g_path.pts[2].uncertain);
  TEST_ASSERT_EQUAL(2, g_path.pts[2].line);
}

// Tank Logo LLD §22.4: GO has no length - the preview draws 1 m straight ahead and every point from there on is
// uncertain (dashed); HOLD draws nothing.
void test_go_draws_one_uncertain_metre() {
  plan("FD 10\nGO\nHOLD RT 90 FD 20");
  TEST_ASSERT_EQUAL(5, g_path.n);  // start, FD 10, GO, RT 90, FD 20
  TEST_ASSERT_FALSE(g_path.pts[1].uncertain);
  TEST_ASSERT_EQUAL_INT32(0, g_path.pts[2].x_mm);
  TEST_ASSERT_EQUAL_INT32(1100, g_path.pts[2].y_mm);  // 10 cm + 1 m ahead
  TEST_ASSERT_TRUE(g_path.pts[2].uncertain);
  TEST_ASSERT_EQUAL(2, g_path.pts[2].line);
  TEST_ASSERT_TRUE(g_path.pts[4].uncertain);  // still a guess after HOLD
}

void test_limits_cut_the_path_short() {
  plan("FOREVER [ ]");
  TEST_ASSERT_TRUE(g_r.cut_short);
  plan("FOREVER [ FD 10 RT 1 ]");
  TEST_ASSERT_TRUE(g_r.cut_short);
  TEST_ASSERT_TRUE(g_r.sim_ms >= 300000 || g_path.n == 2000);
  plan("REPEAT 4 [ FD 50 RT 90 ]", 3);
  TEST_ASSERT_TRUE(g_r.cut_short);
  TEST_ASSERT_EQUAL(3, g_path.n);
}

void test_simulated_time() {
  plan("WAIT 2 SPEED 100 FD 335 WAIT UNTIL DISTANCE < 5");
  TEST_ASSERT_FALSE(g_r.cut_short);
  TEST_ASSERT_INT_WITHIN(150, 2000 + 10000 + 60000, g_r.sim_ms);
}

void test_run_time_error_ends_the_path() {
  plan("FD 10\nFD 5 / 0\nFD 10");
  TEST_ASSERT_EQUAL(7013, g_r.code);
  TEST_ASSERT_EQUAL(2, g_r.line);
  TEST_ASSERT_EQUAL(2, g_path.n);
}

// Plan N3 Task 1: a program that did not compile completely is never planned.
void test_a_failed_compile_is_not_planned() {
  plan("FD 10");
  g_prog.ok = false;
  g_r = Simulator(g_vm).plan(g_prog, g_path);
  TEST_ASSERT_EQUAL(7010, g_r.code);
  TEST_ASSERT_EQUAL(0, g_path.n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_square_corners_within_1_mm);
  RUN_TEST(test_arc_and_pivot);
  RUN_TEST(test_home_turns_back_to_the_start_heading);
  RUN_TEST(test_uncertain_after_a_sensor_read);
  RUN_TEST(test_go_draws_one_uncertain_metre);
  RUN_TEST(test_limits_cut_the_path_short);
  RUN_TEST(test_simulated_time);
  RUN_TEST(test_run_time_error_ends_the_path);
  RUN_TEST(test_a_failed_compile_is_not_planned);
  return UNITY_END();
}
