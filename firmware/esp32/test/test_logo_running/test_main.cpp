// Plan N2 Task 7: every v1 run case of logo/spec/tests/running.yaml (generated into cases.h) through the reader,
// the compiler and the RunController with a fake world: moves end at once (or cut short / stuck as the case says),
// turns move the heading exactly, the clock jumps over waits.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
using cq::core::MotionEvent;
using cq::core::PrimitiveEnd;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
Program g_prog;
Vm g_vm, g_launched;

// A list of words in a case ("1 0 N 120", "done not_found stop"): the next one, or false when used up.
struct Words {
  const char* p = "";
  bool next(char* out, size_t cap) {
    while (*p == ' ') ++p;
    if (*p == '\0') return false;
    size_t n = 0;
    while (*p != ' ' && *p != '\0') {
      if (n + 1 < cap) out[n++] = *p;
      ++p;
    }
    out[n] = '\0';
    return true;
  }
};

struct World {
  explicit World(const RunCase& rc) : c(rc) {
    answers.p = rc.answers;
    ends.p = rc.skill_ends;
  }
  Words answers, ends;
  double x_mm = 0, y_mm = 0, heading = 0;  // the run's frame (x right, y ahead, right = +), as Odometry reports it
  const RunCase& c;
  RunTelem tel;
  uint32_t now = 1000;
  uint8_t motion = 0;
  bool going = false;  // a GO drives on (Tank Logo LLD §22): it counts as one motion
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
    w.heading -= a.delta_cdeg / 100.0;  // + = left in the action, right = + in the pose
  } else {
    w.x_mm += a.dist_cm * 10.0 * sin(w.heading * 3.14159265358979 / 180.0);
    w.y_mm += a.dist_cm * 10.0 * cos(w.heading * 3.14159265358979 / 180.0);
  }
  w.tel.pose.x_mm = static_cast<int32_t>(lround(w.x_mm));
  w.tel.pose.y_mm = static_cast<int32_t>(lround(w.y_mm));
  w.tel.pose.heading_deg = static_cast<int16_t>(lround(w.heading));
  const unsigned long long bit = 1ULL << w.motion++;
  PrimitiveEnd e;
  e.reason = (w.c.cut_short & bit) != 0 ? EndReason::kBlocked
                                        : ((w.c.stuck & bit) != 0 ? EndReason::kStalled : EndReason::kDone);
  e.ok = e.reason == EndReason::kDone;
  return rc.onPrimitiveEnd(e);
}

// GO (LLD §22): the first GO of a run of GOs takes a motion number; if the world cuts it short or sticks it,
// its end comes at once, otherwise it drives on until HOLD or the next motion takes over.
RunOut goStarted(World& w, RunController& rc) {
  if (w.going) return RunOut();
  w.going = true;
  const unsigned long long bit = 1ULL << w.motion++;
  if ((w.c.cut_short & bit) == 0 && (w.c.stuck & bit) == 0) return RunOut();
  PrimitiveEnd e;
  e.reason = (w.c.cut_short & bit) != 0 ? EndReason::kBlocked : EndReason::kStalled;
  w.going = false;
  return rc.onPrimitiveEnd(e);
}

// The laptop answers a question, or does not (null / used up: the runner's 300 ms go by).
void answerAsk(World& w, RunController& rc) {
  char tok[16];
  if (!w.answers.next(tok, sizeof tok) || tok[0] == 'N') return;
  rc.onAskAnswer(atoi(tok), w.now);
}

// A skill is running: its end comes at once (the case's next one); "stop" is the STOP key.
RunOut endSkill(World& w, RunController& rc) {
  static const char* const kNames[] = {"done", "not_found", "lost", "outside_area", "no_way", "blocked", "unknown_thing", "stale", "learning"};
  char tok[24];
  if (!w.ends.next(tok, sizeof tok)) return RunOut();
  if (strcmp(tok, "stop") == 0) return rc.onMotionEvent(MotionEvent::kStop);
  for (uint8_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
    if (strcmp(tok, kNames[i]) == 0) return rc.onSkillEnd(static_cast<SkillEnd>(i), w.now);
  }
  TEST_FAIL_MESSAGE(tok);
  return RunOut();
}

RunOut runToEnd(World& w, RunController& rc) {
  RunOut out;
  for (int i = 0; i < 500000 && !out.ended; ++i) {
    if (!rc.ready() && rc.wakeAt() > w.now) w.now = rc.wakeAt();  // nothing to do before then (LAUNCH: either part)
    out = rc.tick(w.tel, w.now);
    note(w, out);
    if (out.act && out.action.kind == ActKind::kGo) {
      const RunOut g = goStarted(w, rc);
      if (g.ended) out = g;
    } else if (out.act && out.action.kind == ActKind::kHold) {
      w.going = false;
    }
    if (out.act && out.action.kind == ActKind::kAsk) answerAsk(w, rc);
    if (rc.state() == RunState::kSkill) {
      w.going = false;
      const RunOut e = endSkill(w, rc);
      if (e.ended) out = e;
    }
    if (rc.state() == RunState::kMotion) {
      w.going = false;
      out = finishMotion(w, rc, out.action);
    }
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
  Caps caps;
  caps.ai = true;  // the AI words are allowed (the checker's own cases are the conformance tests)
  const CheckResult r = Checker(p, c.src, g_t, g_d).check(prog, caps);
  const bool built = r.ok && Compiler(p, c.src, g_t, g_d).compile(prog, r.handles_obstacles, g_prog);
  char why[256];
  snprintf(why, sizeof why, "%s: refused before running: %s", c.name, g_d.first() ? g_d.first()->msg : "");
  TEST_ASSERT_TRUE_MESSAGE(built, why);
  World w(c);
  w.tel.distance_mm = static_cast<uint16_t>(c.distance_cm * 10);
  RunController rc(g_vm, g_launched);
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
