// Plan N2 Tasks 3-4: source → checked tree → instructions → Vm, as a trace of effects (LLD §7-§8, grammar.md §8).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "logo/checker.h"
#include "logo/compiler.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/vm.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
Program g_prog;
Vm g_vm;
char g_trace[2048];
VmStatus g_end;
int g_effects;
int g_until_ticks = 3;  // WAIT UNTIL gives up after this many ticks (the host's 60 s)

void record(const Effect& e) {
  if (e.kind == EffectKind::kTick) return;
  char one[128], v[96];
  snprintf(one, sizeof one, "%s%s", g_trace[0] != '\0' ? " | " : "", effectWord(e.kind));
  for (uint8_t i = 0; i < e.n; ++i) {
    valueText(g_prog, e.args[i], true, v, sizeof v);
    strncat(one, " ", sizeof one - strlen(one) - 1);
    strncat(one, v, sizeof one - strlen(one) - 1);
  }
  strncat(g_trace, one, sizeof g_trace - strlen(g_trace) - 1);
  ++g_effects;
}

// The host's side of an effect: FORWARD brings the wall 10 cm closer; WAIT UNTIL ticks count towards its limit.
void host(const Effect& e, Sensors& s, int& ticks) {
  if (e.kind == EffectKind::kForward) s.distance_milli -= 10000;
  if (e.kind == EffectKind::kUntilStart) ticks = 0;
  if (e.kind == EffectKind::kTick) s.until_over = ++ticks >= g_until_ticks;
}

// False when the program was refused before running (Diagnostics has why).
bool run(const char* src, int32_t distance_cm = 999, bool stepping = false) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_d.clear();
  pool.reset();
  g_trace[0] = '\0';
  g_effects = 0;
  lex(src, static_cast<uint16_t>(strlen(src)), g_t, g_d);
  Node* prog = p.parse(src, g_t);
  Checker k(p, src, g_t, g_d);
  const CheckResult r = k.check(prog, Caps());
  if (!r.ok || !Compiler(p, src, g_t, g_d).compile(prog, r.handles_obstacles, g_prog)) return false;
  g_vm.load(g_prog, 1);
  g_vm.setStepping(stepping);
  Sensors s;
  s.distance_milli = distance_cm * 1000;
  int ticks = 0;
  for (int guard = 0; guard < 200000; ++guard) {
    g_end = g_vm.run(500, s);
    if (g_end == VmStatus::kEffect) {
      record(g_vm.effect());
      host(g_vm.effect(), s, ticks);
    }
    if (g_end != VmStatus::kEffect && g_end != VmStatus::kYield && g_end != VmStatus::kLine) break;
  }
  return true;
}
uint16_t firstCode() { return g_d.first() != nullptr ? g_d.first()->code : 0; }
}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- Task 3: values and effects

void test_effects_in_order_with_their_values() {
  TEST_ASSERT_TRUE(run("FD 50 RT 90 LED GREEN SAY [ well done ] SOUND HORN PLAY [ C4 2 ] BEEP"));
  TEST_ASSERT_EQUAL_STRING("FORWARD 50 | RIGHT 90 | LED GREEN | SAY [well done] | SOUND HORN | PLAY [C4 2] | BEEP",
                           g_trace);
  TEST_ASSERT_EQUAL(VmStatus::kHalt, g_end);
}

void test_precedence_and_negatives() {
  TEST_ASSERT_TRUE(run("FD 2 + 3 * 4 FD (2 + 3) * 4 FD -5 + 10 FD 10 - 2 - 3 FD 7 / 2"));
  TEST_ASSERT_EQUAL_STRING("FORWARD 14 | FORWARD 20 | FORWARD 5 | FORWARD 5 | FORWARD 3.5", g_trace);
}

void test_words_lists_presets_and_inputs() {
  TEST_ASSERT_TRUE(run("SAY \"Done! LOOK [ 30 -10 ] ARC LEFT 90 50 LED [ 255 128 0 ]"));
  TEST_ASSERT_EQUAL_STRING("SAY Done! | LOOK [30 -10] | ARC LEFT 90 50 | LED [255 128 0]", g_trace);
}

void test_sensors_and_reporters() {
  TEST_ASSERT_TRUE(run("FD DISTANCE / 2 FD ABS -3 FD ROUND 2.5 FD REMAINDER 7 3 FD REPCOUNT + 1", 30));
  TEST_ASSERT_EQUAL_STRING("FORWARD 15 | FORWARD 3 | FORWARD 3 | FORWARD 1 | FORWARD 1", g_trace);
}

void test_run_time_error_has_its_line() {
  TEST_ASSERT_TRUE(run("FD 10\nFD 5 / 0"));
  TEST_ASSERT_EQUAL_STRING("FORWARD 10", g_trace);
  TEST_ASSERT_EQUAL(VmStatus::kError, g_end);
  TEST_ASSERT_EQUAL(7013, g_vm.error().code);
  TEST_ASSERT_EQUAL(2, g_vm.error().line);
}

void test_comparisons_give_true_or_false() {
  TEST_ASSERT_TRUE(run("SAY (\"cat = \"CAT) SAY (NOT (1 = 2) AND 3 >= 3) SAY (1 > 2 OR FALSE)"));
  TEST_ASSERT_EQUAL_STRING("SAY TRUE | SAY TRUE | SAY FALSE", g_trace);
}

void test_a_program_too_big_to_compile_is_7010() {
  static char src[6000];
  src[0] = '\0';
  for (int i = 0; i < 1100; ++i) strcat(src, "BEEP ");
  TEST_ASSERT_FALSE(run(src));
  TEST_ASSERT_EQUAL(7010, firstCode());
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "too many steps"));
}

void test_your_word_without_loops() {
  TEST_ASSERT_TRUE(run("TO STEP :d FD :d RT 90 END\nSTEP 10 STEP 5 * 4"));
  TEST_ASSERT_EQUAL_STRING("FORWARD 10 | RIGHT 90 | FORWARD 20 | RIGHT 90", g_trace);
}

void test_deep_calls_end_with_7011() {
  TEST_ASSERT_TRUE(run("TO D :n FD 1 D :n + 1 BEEP END D 0"));
  TEST_ASSERT_EQUAL(VmStatus::kError, g_end);
  TEST_ASSERT_EQUAL(7011, g_vm.error().code);
  TEST_ASSERT_EQUAL(100, g_effects);
}

void test_step_mode_stops_at_each_line() {
  TEST_ASSERT_TRUE(run("FD 1\nRT 2\n\nBEEP", 999, true));
  TEST_ASSERT_EQUAL_STRING("FORWARD 1 | RIGHT 2 | BEEP", g_trace);
  TEST_ASSERT_EQUAL(4, g_vm.line());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_effects_in_order_with_their_values);
  RUN_TEST(test_precedence_and_negatives);
  RUN_TEST(test_words_lists_presets_and_inputs);
  RUN_TEST(test_sensors_and_reporters);
  RUN_TEST(test_run_time_error_has_its_line);
  RUN_TEST(test_comparisons_give_true_or_false);
  RUN_TEST(test_a_program_too_big_to_compile_is_7010);
  RUN_TEST(test_your_word_without_loops);
  RUN_TEST(test_deep_calls_end_with_7011);
  RUN_TEST(test_step_mode_stops_at_each_line);
  return UNITY_END();
}
