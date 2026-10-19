// Plan N2 Task 7: every v1 run case of logo/spec/tests/running.yaml (generated into cases.h) through the reader,
// the compiler and the RunController with a fake world: moves end at once (or cut short / stuck as the case says),
// turns move the heading exactly, the clock jumps over waits.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "cases.h"
#include "logo/checker.h"
#include "logo/compiler.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/run_controller.h"

using namespace cq::logo;
using cq::core::EndReason;
using cq::core::PrimitiveEnd;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
Program g_prog;
Vm g_vm;

struct World {
  explicit World(const RunCase& rc) : c(rc) {}
  const RunCase& c;
  RunTelem tel;
  uint32_t now = 1000;
  uint8_t motion = 0;
  char trace[2048] = "";
};

void note(World& w, const RunOut& o) {
  if (!o.act) return;
  char one[160];
  actionText(o.action, one, sizeof one);
  if (w.trace[0] != '\0') strncat(w.trace, " | ", sizeof w.trace - strlen(w.trace) - 1);
  strncat(w.trace, one, sizeof w.trace - strlen(w.trace) - 1);
}

RunOut finishMotion(World& w, RunController& rc, const Action& a) {
  if (a.kind != ActKind::kMove) {
    w.tel.yaw_cdeg = static_cast<uint16_t>((w.tel.yaw_cdeg + 36000 + a.delta_cdeg % 36000) % 36000);
  }
  const unsigned long long bit = 1ULL << w.motion++;
  PrimitiveEnd e;
  e.reason = (w.c.cut_short & bit) != 0 ? EndReason::kBlocked
                                        : ((w.c.stuck & bit) != 0 ? EndReason::kStalled : EndReason::kDone);
  e.ok = e.reason == EndReason::kDone;
  return rc.onPrimitiveEnd(e);
}

RunOut runToEnd(World& w, RunController& rc) {
  RunOut out;
  for (int i = 0; i < 500000 && !out.ended; ++i) {
    if (rc.state() == RunState::kWaiting && rc.wakeAt() > w.now) w.now = rc.wakeAt();
    out = rc.tick(w.tel, w.now);
    note(w, out);
    if (rc.state() == RunState::kMotion) out = finishMotion(w, rc, out.action);
    w.now += 2;
  }
  return out;
}

void runCase(const RunCase& c) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_d.clear();
  pool.reset();
  lex(c.src, static_cast<uint16_t>(strlen(c.src)), g_t, g_d);
  Node* prog = p.parse(c.src, g_t);
  const CheckResult r = Checker(p, c.src, g_t, g_d).check(prog, Caps());
  const bool built = r.ok && Compiler(p, c.src, g_t, g_d).compile(prog, r.handles_obstacles, g_prog);
  char why[256];
  snprintf(why, sizeof why, "%s: refused before running: %s", c.name, g_d.first() ? g_d.first()->msg : "");
  TEST_ASSERT_TRUE_MESSAGE(built, why);
  World w(c);
  w.tel.distance_mm = static_cast<uint16_t>(c.distance_cm * 10);
  RunController rc(g_vm);
  rc.start(g_prog, RunConfig(), w.tel, w.now);
  const RunOut end = runToEnd(w, rc);
  TEST_ASSERT_TRUE_MESSAGE(end.ended, c.name);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(c.does, w.trace, c.name);
  snprintf(why, sizeof why, "%s: ended %u on line %u: %s", c.name, end.code, end.end_line, end.msg);
  TEST_ASSERT_EQUAL_MESSAGE(c.end_code, end.code, why);
  if (c.end_code != 0) TEST_ASSERT_EQUAL_MESSAGE(c.end_line, end.end_line, why);
  if (c.message_has[0] != '\0') TEST_ASSERT_NOT_NULL_MESSAGE(strstr(end.msg, c.message_has), why);
}
}  // namespace

void setUp() {}
void tearDown() {}

#define DEFINE_CASE(i) \
  void test_run_case_##i() { runCase(kRunCases[i]); }
LOGO_RUN_CASES(DEFINE_CASE)

int main(int, char**) {
  UNITY_BEGIN();
#define RUN_CASE(i) RUN_TEST(test_run_case_##i);
  LOGO_RUN_CASES(RUN_CASE)
  return UNITY_END();
}
