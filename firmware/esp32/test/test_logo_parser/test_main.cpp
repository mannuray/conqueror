// Plan N1 Tasks 4-5: pass 1 (definitions) and pass 2 (statements, inputs, expressions) + the reads printer.
#include <string.h>
#include <unity.h>

#include "logo/lexer.h"
#include "logo/parser.h"
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pass1_finds_definitions_before_use);
  RUN_TEST(test_pass1_errors);
  RUN_TEST(test_edit_distance);
  return UNITY_END();
}
