// Plan N1 Task 7: every v1 case of logo/spec/tests (reading.yaml, errors.yaml), generated into cases.h.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "cases.h"
#include "logo/checker.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/printer.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];

void yourWords(const Parser& p, const char* src, char* out, size_t cap) {
  out[0] = '\0';
  for (uint8_t i = 0; i < p.procCount(); ++i) {
    const ProcInfo& pi = p.proc(i);
    if (!pi.valid) continue;
    char nm[40], one[64];
    const Token& t = g_t.t[pi.name_tok];
    for (uint16_t k = 0; k < t.len && k + 1 < sizeof nm; ++k) {
      const char c = src[t.start + k];
      nm[k] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
      nm[k + 1] = '\0';
    }
    snprintf(one, sizeof one, "%s:%u:%d;", nm, pi.n_inputs, pi.gives_value ? 1 : 0);
    strncat(out, one, cap - strlen(out) - 1);
  }
}

void tailNames(const Parser& p, const char* src, uint64_t bits, char* out, size_t cap) {
  // sorted, comma-separated upper-case names (like logogen)
  char names[64][40];
  uint8_t n = 0;
  for (uint8_t i = 0; i < p.procCount() && i < 64; ++i) {
    if ((bits & (1ULL << i)) == 0) continue;
    const Token& t = g_t.t[p.proc(i).name_tok];
    uint16_t k = 0;
    for (; k < t.len && k < 39; ++k) {
      const char c = src[t.start + k];
      names[n][k] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    names[n++][k] = '\0';
  }
  for (uint8_t i = 1; i < n; ++i) {  // insertion sort
    for (uint8_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
      char tmp[40];
      strcpy(tmp, names[j]);
      strcpy(names[j], names[j - 1]);
      strcpy(names[j - 1], tmp);
    }
  }
  out[0] = '\0';
  for (uint8_t i = 0; i < n; ++i) {
    if (i > 0) strncat(out, ",", cap - strlen(out) - 1);
    strncat(out, names[i], cap - strlen(out) - 1);
  }
}

void runCase(const LogoCase& c) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  char why[256];
  g_d.clear();
  pool.reset();
  lex(c.src, static_cast<uint16_t>(strlen(c.src)), g_t, g_d);
  Node* prog = p.parse(c.src, g_t);
  CheckResult r;
  if (c.check) {
    Caps caps;
    caps.encoders = c.encoders;
    Checker k(p, c.src, g_t, g_d);
    r = k.check(prog, caps);
  }
  const Diag* f = g_d.first();
  if (c.expect_ok) {
    snprintf(why, sizeof why, "%s: unexpected %u at %u:%u %s", c.name, f ? f->code : 0, f ? f->line : 0,
             f ? f->col : 0, f ? f->msg : "");
    TEST_ASSERT_NULL_MESSAGE(f, why);
    static char out[4096];
    printReads(prog, c.src, g_t, p, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reads, out, c.name);
    if (c.your_words[0] != '\0') {
      char yw[256];
      yourWords(p, c.src, yw, sizeof yw);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(c.your_words, yw, c.name);
    }
    if (c.handles_obstacles >= 0) TEST_ASSERT_EQUAL_MESSAGE(c.handles_obstacles, r.handles_obstacles ? 1 : 0, c.name);
    if (strcmp(c.tail_calls, "-") != 0) {  // "-" = not checked
      char tn[256];
      tailNames(p, c.src, r.tail_callees, tn, sizeof tn);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(c.tail_calls, tn, c.name);
    }
    return;
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(f, c.name);
  snprintf(why, sizeof why, "%s: got %u at %u:%u %s", c.name, f->code, f->line, f->col, f->msg);
  TEST_ASSERT_EQUAL_MESSAGE(c.code, f->code, why);
  TEST_ASSERT_EQUAL_MESSAGE(c.line, f->line, why);
  TEST_ASSERT_EQUAL_MESSAGE(c.col, f->col, why);
  if (c.message_has[0] != '\0') TEST_ASSERT_NOT_NULL_MESSAGE(strstr(f->msg, c.message_has), why);
}
}  // namespace

void setUp() {}
void tearDown() {}

#define DEFINE_CASE(i) \
  void test_case_##i() { runCase(kCases[i]); }
LOGO_CASES(DEFINE_CASE)

int main(int, char**) {
  UNITY_BEGIN();
#define RUN_CASE(i) RUN_TEST(test_case_##i);
  LOGO_CASES(RUN_CASE)
  return UNITY_END();
}
