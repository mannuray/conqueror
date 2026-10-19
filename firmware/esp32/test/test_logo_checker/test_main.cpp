// Plan N1 Task 6: scope, needs, level, ranges, OUTPUT, ?, BLOCKED?, tail positions (grammar.md §8, §11).
#include <string.h>
#include <unity.h>

#include "logo/checker.h"
#include "logo/lexer.h"
#include "logo/parser.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
CheckResult C(const char* s, Caps caps = Caps()) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_d.clear();
  pool.reset();
  lex(s, static_cast<uint16_t>(strlen(s)), g_t, g_d);
  Node* prog = p.parse(s, g_t);
  Checker k(p, s, g_t, g_d);
  return k.check(prog, caps);
}
uint16_t first() { return g_d.first() ? g_d.first()->code : 0; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_check_scope() {
  C("TO SQUARE :size REPEAT 4 [ FD :sise RT 90 ] END");
  TEST_ASSERT_EQUAL(7009, first());
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, ":size"));
  C("TO A :x FD :x END\nFD :x");
  TEST_ASSERT_EQUAL(7009, first());
  TEST_ASSERT_EQUAL(2, g_d.first()->line);
}

void test_check_needs_before_level() {
  C("IF SEE? \"ball [ BEEP ]");
  TEST_ASSERT_EQUAL(7020, first());
  C("FD FIRST [ 10 20 ]");
  TEST_ASSERT_EQUAL(7030, first());
  Caps no_enc;
  no_enc.encoders = false;
  C("FD 50", no_enc);
  TEST_ASSERT_EQUAL(7021, first());
  TEST_ASSERT_TRUE(C("TO D :x OUTPUT :x * 2 END FD D 5").ok);  // OUTPUT is in v1
}

void test_check_ranges_and_arc() {
  C("FD 600");
  TEST_ASSERT_EQUAL(7025, first());
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "1 to 500 cm"));
  C("ARC RIGHT 90 20");
  TEST_ASSERT_EQUAL(7022, first());
  TEST_ASSERT_EQUAL(14, g_d.first()->col);
  TEST_ASSERT_TRUE(C("FD 2.5 RT 720").ok);
}

void test_check_output_placeholder_blocked() {
  C("OUTPUT 5");
  TEST_ASSERT_EQUAL(7026, first());
  C("FD ?");
  TEST_ASSERT_EQUAL(7006, first());
  TEST_ASSERT_TRUE(C("FOREVER [ FD 50 IF BLOCKED? [ BK 10 RT 90 ] ]").handles_obstacles);
  TEST_ASSERT_FALSE(C("FD 100").handles_obstacles);
}

void test_check_tail_positions() {
  CheckResult r = C("TO COUNTDOWN :n IFELSE :n = 0 [ BEEP ] [ FD 10 COUNTDOWN :n - 1 ] END COUNTDOWN 500");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.tail_callees == 1);  // proc 0
  r = C("TO A :n REPEAT 2 [ A :n - 1 ] END");
  TEST_ASSERT_TRUE(r.tail_callees == 0);
  r = C("TO PING :n IF :n > 0 [ RT 10 PONG :n - 1 ] END TO PONG :n IF :n > 0 [ LT 10 PING :n - 1 ] END PING 3");
  TEST_ASSERT_TRUE(r.tail_callees == 3);
  r = C("TO SUM :n :acc IFELSE :n = 0 [ OUTPUT :acc ] [ OUTPUT SUM :n - 1 :acc + :n ] END FD SUM 20 0");
  TEST_ASSERT_TRUE(r.tail_callees == 1);
}

// Final review I1: a long program is checked to its last statement (no silent skip when the walk is wide).
void test_check_long_program_checks_every_statement() {
  static char src[2200];
  src[0] = '\0';
  for (int i = 0; i < 300; ++i) strcat(src, "BEEP\n");
  strcat(src, "FD 50");
  Caps no_enc;
  no_enc.encoders = false;
  TEST_ASSERT_FALSE(C(src, no_enc).ok);
  TEST_ASSERT_EQUAL(7021, first());
  TEST_ASSERT_EQUAL(301, g_d.first()->line);
}

// A tree deeper than the walk stack (a 300-term sum) is refused with 7010, never passed unchecked.
void test_check_too_deep_tree_is_7010() {
  static char src[1400];
  strcpy(src, "FD 1");
  for (int i = 0; i < 300; ++i) strcat(src, " + 1");
  TEST_ASSERT_FALSE(C(src).ok);
  TEST_ASSERT_EQUAL(7010, first());
}

// Plan N2 Task 1: a PLAY list is checked before anything moves.
void test_check_play_notes() {
  TEST_ASSERT_TRUE(C("PLAY [ C4 E4 2 G4 R C#5 0.5 ]").ok);
  C("PLAY [ C4 H4 ]");
  TEST_ASSERT_EQUAL(7029, first());
  TEST_ASSERT_EQUAL(11, g_d.first()->col);
  C("PLAY [ 2 C4 ]");
  TEST_ASSERT_EQUAL(7029, first());  // a length needs a note before it
  C("PLAY [ C4 2 3 ]");
  TEST_ASSERT_EQUAL(7029, first());  // two lengths in a row
  C("PLAY [ C4 0.1 ]");
  TEST_ASSERT_EQUAL(7025, first());
  C("PLAY [ ]");
  TEST_ASSERT_EQUAL(7025, first());
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "1 to 64 notes"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_check_scope);
  RUN_TEST(test_check_needs_before_level);
  RUN_TEST(test_check_ranges_and_arc);
  RUN_TEST(test_check_output_placeholder_blocked);
  RUN_TEST(test_check_tail_positions);
  RUN_TEST(test_check_long_program_checks_every_statement);
  RUN_TEST(test_check_too_deep_tree_is_7010);
  RUN_TEST(test_check_play_notes);
  return UNITY_END();
}
