// Plan N1 Tasks 4-5: pass 1 (definitions) and pass 2 (statements, inputs, expressions) + the reads printer.
#include <string.h>
#include <unity.h>

#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/printer.h"
#include "logo/words.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
const char* g_src = "";
Parser* g_p = nullptr;
NodePool* g_pool = nullptr;
Node* parseText(const char* s) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_src = s;
  g_d.clear();
  pool.reset();
  lex(s, static_cast<uint16_t>(strlen(s)), g_t, g_d);
  g_p = &p;
  g_pool = &pool;
  return p.parse(s, g_t);
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_pass1_finds_definitions_before_use() {
  parseText("SQUARE 30\nTO SQUARE :s REPEAT 4 [ FD :s RT 90 ] END\nTO SIDE OUTPUT 50 END");
  TEST_ASSERT_EQUAL(2, g_p->procCount());
  TEST_ASSERT_EQUAL(1, g_p->proc(0).n_inputs);
  TEST_ASSERT_FALSE(g_p->proc(0).gives_value);
  TEST_ASSERT_TRUE(g_p->proc(1).gives_value);
  TEST_ASSERT_EQUAL(0, g_d.errorCount());
}

void test_pass1_errors() {
  parseText("TO A FD 10 END\nTO A FD 20 END");
  TEST_ASSERT_EQUAL(7007, g_d.first()->code);
  parseText("TO FORWARD :n BK :n END");
  TEST_ASSERT_EQUAL(7008, g_d.first()->code);
  parseText("TO RED BEEP END");
  TEST_ASSERT_EQUAL(7008, g_d.first()->code);
  parseText("REPEAT 2 [ TO A BEEP END ]");
  TEST_ASSERT_EQUAL(7017, g_d.first()->code);
  TEST_ASSERT_EQUAL(12, g_d.first()->col);
  parseText("TO SQUARE :s FD :s");
  TEST_ASSERT_EQUAL(7003, g_d.first()->code);
  TEST_ASSERT_EQUAL(1, g_d.first()->col);
}

void test_edit_distance() {
  TEST_ASSERT_EQUAL(2, editDistance("SQAURE", 6, "SQUARE", 6));
  TEST_ASSERT_EQUAL(1, editDistance("sise", 4, "size", 4));
  TEST_ASSERT_EQUAL(3, editDistance("A", 1, "FORWARD", 7));  // capped
}

namespace {
const char* R(const char* s) {
  static char out[4096];
  Node* prog = parseText(s);
  printReads(prog, g_src, g_t, *g_p, out, sizeof out);
  return out;
}
}  // namespace

void test_reads_simple_statements() {
  TEST_ASSERT_EQUAL_STRING("(FORWARD 50) (RIGHT 90)", R("fd 50 RT 90"));
  TEST_ASSERT_EQUAL_STRING("(REPEAT 4 [(FORWARD 50) (RIGHT 90)])", R("REPEAT 4 [ FD 50 RT 90 ]"));
  TEST_ASSERT_EQUAL(0, g_d.errorCount());
}

void test_reads_expressions_and_precedence() {
  TEST_ASSERT_EQUAL_STRING("(FORWARD (+ 2 (* 3 4)))", R("FD 2 + 3 * 4"));
  TEST_ASSERT_EQUAL_STRING("(IF (= (RANDOM 2) 1) [(BEEP)])", R("IF RANDOM 2 = 1 [ BEEP ]"));
  TEST_ASSERT_EQUAL_STRING("(IF (OR (AND (NOT (OBSTACLE?)) (> (DISTANCE) 50)) (> (TIMER) 10)) [(BEEP)])",
                           R("IF NOT OBSTACLE? AND DISTANCE > 50 OR TIMER > 10 [ BEEP ]"));
  TEST_ASSERT_EQUAL_STRING("(LOOK {-30 10})", R("LOOK [ -30 10 ]"));
}

void test_reads_definitions_presets_wait_until() {
  TEST_ASSERT_EQUAL_STRING("(TO SIDE () [(OUTPUT 50)]) (FORWARD (SIDE))", R("TO SIDE OUTPUT 50 END FD SIDE"));
  TEST_ASSERT_EQUAL_STRING("(ARC RIGHT 90 50) (PIVOT LEFT 90) (LEFT 45)", R("ARC RIGHT 90 50 PIVOT LEFT 90 LEFT 45"));
  TEST_ASSERT_EQUAL_STRING("(WAIT-UNTIL (< (DISTANCE) 20)) (BEEP)", R("WAIT UNTIL DISTANCE < 20 BEEP"));
}

void test_parser_minus_cases() {
  R("FD 10 -5");
  TEST_ASSERT_EQUAL(7004, g_d.first()->code);
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "10 - 5"));
  R("FD (10 -5)");
  TEST_ASSERT_EQUAL(7028, g_d.first()->code);
  R("REPEAT 10 -5 [ BEEP ]");
  TEST_ASSERT_EQUAL(7023, g_d.first()->code);
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "10 - 5"));
}

void test_parser_empty_program() {
  TEST_ASSERT_EQUAL_STRING("", R("  ; only a comment\n"));
  TEST_ASSERT_EQUAL(0, g_d.count());
}

void test_parser_deep_nesting_is_an_error() {
  static char s[600];
  strcpy(s, "FD ");
  for (int i = 0; i < 200; ++i) strcat(s, "(");
  strcat(s, "1");
  R(s);
  TEST_ASSERT_EQUAL(7010, g_d.first()->code);
}

void test_parser_node_pool_full() {
  // nodes and tokens are about one to one, so the token cap is hit first with the real pool: use a small pool
  static Node small[50];
  NodePool pool(small, 50);
  Diagnostics d;
  Parser p(pool, d);
  static char s[300];
  s[0] = '\0';
  for (int i = 0; i < 40; ++i) strcat(s, "FD 1 ");  // 80 nodes
  lex(s, static_cast<uint16_t>(strlen(s)), g_t, d);
  p.parse(s, g_t);
  TEST_ASSERT_EQUAL(7010, d.first()->code);
}

void test_parser_always_terminates_on_garbage() {
  const char* bad[] = {"]", "[", ")", "(", "END", "TO", "TO X", "IF", "IFELSE [", "FD", "FD ]", "( ( (", "] ] ]",
                       "REPEAT 4 [ ] ]", "+ - * /", "FD 1 + ", "IF 1 < [ ]", "LED", "LED [", "WAIT UNTIL",
                       "TO A :x A :x", "\"", ":", "? ?1 ?2", "[ [ [ 1 ] ] ]"};
  for (const char* b : bad) R(b);  // must return; every case finishing is the test
  TEST_PASS();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pass1_finds_definitions_before_use);
  RUN_TEST(test_pass1_errors);
  RUN_TEST(test_edit_distance);
  RUN_TEST(test_reads_simple_statements);
  RUN_TEST(test_reads_expressions_and_precedence);
  RUN_TEST(test_reads_definitions_presets_wait_until);
  RUN_TEST(test_parser_minus_cases);
  RUN_TEST(test_parser_empty_program);
  RUN_TEST(test_parser_deep_nesting_is_an_error);
  RUN_TEST(test_parser_node_pool_full);
  RUN_TEST(test_parser_always_terminates_on_garbage);
  return UNITY_END();
}
