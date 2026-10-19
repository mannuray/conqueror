// Plan N2 Task 2: the Vm on hand-built programs - arithmetic, types, jumps, loops, calls and tail calls, budget,
// step mode, run-time errors with their line (LLD §7-§8).
#include <string.h>
#include <unity.h>

#include "logo/vm.h"

using namespace cq::logo;

namespace {
Program g_p;
Vm g_vm;
Sensors g_s;

void reset() {
  g_p.clear();
  g_s = Sensors();
}
void emit(OpCode op, uint8_t a = 0, int32_t n = 0) { g_p.code[g_p.n_code++] = Instr{op, a, n}; }
void opr(Op o) { emit(OpCode::kOp, static_cast<uint8_t>(o)); }
void fx(EffectKind k, int32_t n) { emit(OpCode::kEffect, static_cast<uint8_t>(k), n); }
VmStatus start(uint16_t budget = 500) {
  g_vm.load(g_p, 7);
  return g_vm.run(budget, g_s);
}
int32_t arg0() { return g_vm.effect().args[0].n; }
}  // namespace

void setUp() { reset(); }
void tearDown() {}

void test_vm_arithmetic_and_effect() {
  emit(OpCode::kLine, 0, 3);
  emit(OpCode::kPushN, 0, 2000);
  emit(OpCode::kPushN, 0, 3000);
  emit(OpCode::kPushN, 0, 4500);
  opr(Op::kMul);
  opr(Op::kAdd);
  fx(EffectKind::kForward, 1);
  emit(OpCode::kHalt);
  TEST_ASSERT_EQUAL(VmStatus::kEffect, start());
  TEST_ASSERT_EQUAL(EffectKind::kForward, g_vm.effect().kind);
  TEST_ASSERT_EQUAL(15500, arg0());
  TEST_ASSERT_EQUAL(3, g_vm.effect().line);
  TEST_ASSERT_EQUAL(VmStatus::kHalt, g_vm.run(500, g_s));
  TEST_ASSERT_EQUAL(VmStatus::kHalt, g_vm.run(500, g_s));  // stays finished
}

void test_vm_division_rounding_and_negatives() {
  emit(OpCode::kPushN, 0, -7000);
  emit(OpCode::kPushN, 0, 2000);
  opr(Op::kDiv);  // -3.5
  emit(OpCode::kCallB, static_cast<uint8_t>(Builtin::kRound));  // -4 (halves away from zero)
  emit(OpCode::kPushN, 0, -7000);
  emit(OpCode::kPushN, 0, 2000);
  emit(OpCode::kCallB, static_cast<uint8_t>(Builtin::kRemainder));  // -1
  emit(OpCode::kPushN, 0, 1000);
  emit(OpCode::kPushN, 0, 3000);
  opr(Op::kDiv);  // 0.333
  fx(EffectKind::kArc, 3);
  TEST_ASSERT_EQUAL(VmStatus::kEffect, start());
  TEST_ASSERT_EQUAL(-4000, g_vm.effect().args[0].n);
  TEST_ASSERT_EQUAL(-1000, g_vm.effect().args[1].n);
  TEST_ASSERT_EQUAL(333, g_vm.effect().args[2].n);
}

void test_vm_divide_by_zero_and_too_big_have_the_line() {
  emit(OpCode::kLine, 0, 4);
  emit(OpCode::kPushN, 0, 5000);
  emit(OpCode::kPushN, 0, 0);
  opr(Op::kDiv);
  TEST_ASSERT_EQUAL(VmStatus::kError, start());
  TEST_ASSERT_EQUAL(7013, g_vm.error().code);
  TEST_ASSERT_EQUAL(4, g_vm.error().line);
  TEST_ASSERT_EQUAL_STRING("Can't divide by zero on line 4.", g_vm.error().msg);
  reset();
  emit(OpCode::kLine, 0, 2);
  emit(OpCode::kPushN, 0, 1000000000);
  emit(OpCode::kPushN, 0, 2000);
  opr(Op::kMul);
  TEST_ASSERT_EQUAL(VmStatus::kError, start());
  TEST_ASSERT_EQUAL(7033, g_vm.error().code);
}

void test_vm_type_errors_name_the_word_and_value() {
  const int16_t cat = g_p.addWord("cat", 3);
  emit(OpCode::kPushC, 0, cat);
  emit(OpCode::kPushN, 0, 1000);
  opr(Op::kAdd);
  TEST_ASSERT_EQUAL(VmStatus::kError, start());
  TEST_ASSERT_EQUAL(7012, g_vm.error().code);
  TEST_ASSERT_EQUAL_STRING("+ needs a number, but got cat.", g_vm.error().msg);
  reset();
  emit(OpCode::kPushN, 0, 5000);
  emit(OpCode::kJz, 0, 0);  // IF 5 [ ]
  TEST_ASSERT_EQUAL(VmStatus::kError, start());
  TEST_ASSERT_EQUAL_STRING("IF needs TRUE or FALSE, but got 5.", g_vm.error().msg);
}

void test_vm_equality_of_words_and_lists() {
  const int16_t a = g_p.addWord("Cat", 3), b = g_p.addWord("cAT", 3);
  const Value items[] = {{VTag::kNum, 1000}, {VTag::kWord, a}};
  const int16_t l1 = g_p.addList(items, 2), l2 = g_p.addList(items, 2);
  emit(OpCode::kPushC, 0, a);
  emit(OpCode::kPushC, 0, b);
  opr(Op::kEq);
  emit(OpCode::kPushC, 0, l1);
  emit(OpCode::kPushC, 0, l2);
  opr(Op::kNe);
  emit(OpCode::kPushN, 0, 2000);
  emit(OpCode::kPushN, 0, 3000);
  opr(Op::kLe);
  fx(EffectKind::kArc, 3);
  TEST_ASSERT_EQUAL(VmStatus::kEffect, start());
  TEST_ASSERT_EQUAL(1, g_vm.effect().args[0].n);  // "Cat = "cAT
  TEST_ASSERT_EQUAL(0, g_vm.effect().args[1].n);  // equal lists are not <>
  TEST_ASSERT_EQUAL(1, g_vm.effect().args[2].n);
}

void test_vm_repeat_rounds_and_repcount() {
  emit(OpCode::kPushN, 0, 3500);  // REPEAT 3.5 = 3 rounds
  emit(OpCode::kLoopInit, 0);
  emit(OpCode::kLoopNext, 0, 6);  // pc 2
  emit(OpCode::kCallB, static_cast<uint8_t>(Builtin::kRepcount));
  fx(EffectKind::kForward, 1);
  emit(OpCode::kJmp, 0, 2);
  emit(OpCode::kHalt);  // pc 6
  int32_t seen[4] = {0, 0, 0, 0};
  VmStatus st = start();
  for (int i = 0; i < 4 && st == VmStatus::kEffect; ++i) {
    seen[i] = arg0();
    st = g_vm.run(500, g_s);
  }
  TEST_ASSERT_EQUAL(VmStatus::kHalt, st);
  TEST_ASSERT_EQUAL(1000, seen[0]);
  TEST_ASSERT_EQUAL(3000, seen[2]);
  TEST_ASSERT_EQUAL(0, seen[3]);
  TEST_ASSERT_EQUAL(0, g_vm.round());  // outside any loop
}

void test_vm_repeat_count_out_of_range() {
  emit(OpCode::kPushN, 0, -1000);
  emit(OpCode::kLoopInit, 0);
  TEST_ASSERT_EQUAL(VmStatus::kError, start());
  TEST_ASSERT_EQUAL(7025, g_vm.error().code);
  TEST_ASSERT_EQUAL_STRING("REPEAT needs how many times from 0 to 10000.", g_vm.error().msg);
}

void test_vm_empty_forever_yields_at_the_budget() {
  emit(OpCode::kLoopInit, 1);
  emit(OpCode::kLoopNext, 0, 3);  // pc 1
  emit(OpCode::kJmp, 0, 1);
  emit(OpCode::kHalt);
  TEST_ASSERT_EQUAL(VmStatus::kYield, start(500));
  TEST_ASSERT_EQUAL(500, g_vm.executed());
  TEST_ASSERT_EQUAL(VmStatus::kYield, g_vm.run(500, g_s));
  TEST_ASSERT_EQUAL(1000, g_vm.executed());
}

// TO DOUBLE :x OUTPUT :x * 2 END ; FD DOUBLE 21
void test_vm_call_with_input_and_output() {
  g_p.procs[0] = ProcEntry{5, 1, true, static_cast<uint16_t>(g_p.addWord("DOUBLE", 6))};
  g_p.n_procs = 1;
  emit(OpCode::kPushN, 0, 21000);
  emit(OpCode::kCall, 1, 0);
  fx(EffectKind::kForward, 1);
  emit(OpCode::kHalt);
  emit(OpCode::kHalt);  // pc 4 (unused)
  emit(OpCode::kArg, 0);  // pc 5
  emit(OpCode::kPushN, 0, 2000);
  opr(Op::kMul);
  emit(OpCode::kRetV);
  TEST_ASSERT_EQUAL(VmStatus::kEffect, start());
  TEST_ASSERT_EQUAL(42000, arg0());
}

// TO COUNTDOWN :n IF :n = 0 [ STOP-like HALT ] COUNTDOWN :n - 1 (tail) END ; COUNTDOWN 5000
void test_vm_tail_calls_use_one_frame() {
  g_p.procs[0] = ProcEntry{3, 1, false, static_cast<uint16_t>(g_p.addWord("COUNTDOWN", 9))};
  g_p.n_procs = 1;
  emit(OpCode::kPushN, 0, 5000000);
  emit(OpCode::kCall, 1, 0);
  emit(OpCode::kHalt);
  emit(OpCode::kArg, 0);  // pc 3
  emit(OpCode::kPushN, 0, 0);
  opr(Op::kEq);
  emit(OpCode::kJz, 0, 9);
  emit(OpCode::kRet);
  emit(OpCode::kHalt);  // pc 8 (unused)
  emit(OpCode::kArg, 0);  // pc 9
  emit(OpCode::kPushN, 0, 1000);
  opr(Op::kSub);
  emit(OpCode::kTCall, 1, 0);
  VmStatus st = start(60000);
  while (st == VmStatus::kYield) st = g_vm.run(60000, g_s);
  TEST_ASSERT_EQUAL(VmStatus::kHalt, st);
}

// TO DEEP :n DEEP :n + 1 (not a tail call: BEEP after it) END
void test_vm_deep_calls_are_7011() {
  g_p.procs[0] = ProcEntry{3, 1, false, static_cast<uint16_t>(g_p.addWord("DEEP", 4))};
  g_p.n_procs = 1;
  emit(OpCode::kPushN, 0, 0);
  emit(OpCode::kCall, 1, 0);
  emit(OpCode::kHalt);
  emit(OpCode::kArg, 0);  // pc 3
  emit(OpCode::kCall, 1, 0);
  fx(EffectKind::kBeep, 0);
  emit(OpCode::kRet);
  TEST_ASSERT_EQUAL(VmStatus::kError, start(5000));
  TEST_ASSERT_EQUAL(7011, g_vm.error().code);
}

// OUTPUT from inside a REPEAT leaves the word's loop: the caller's REPCOUNT is its own again.
void test_vm_output_inside_a_loop_pops_the_loop() {
  g_p.procs[0] = ProcEntry{9, 0, true, static_cast<uint16_t>(g_p.addWord("FIRSTROUND", 10))};
  g_p.n_procs = 1;
  emit(OpCode::kPushN, 0, 2000);  // REPEAT 2 [ FD FIRSTROUND + REPCOUNT ]
  emit(OpCode::kLoopInit, 0);
  emit(OpCode::kLoopNext, 0, 8);  // pc 2
  emit(OpCode::kCall, 0, 0);
  emit(OpCode::kCallB, static_cast<uint8_t>(Builtin::kRepcount));
  opr(Op::kAdd);
  fx(EffectKind::kForward, 1);
  emit(OpCode::kJmp, 0, 2);
  emit(OpCode::kHalt);  // pc 8
  emit(OpCode::kPushN, 0, 5000);  // pc 9: REPEAT 5 [ OUTPUT REPCOUNT * 100 ]
  emit(OpCode::kLoopInit, 0);
  emit(OpCode::kLoopNext, 0, 17);  // pc 11
  emit(OpCode::kCallB, static_cast<uint8_t>(Builtin::kRepcount));
  emit(OpCode::kPushN, 0, 100000);
  opr(Op::kMul);
  emit(OpCode::kRetV);
  emit(OpCode::kJmp, 0, 11);
  emit(OpCode::kFail, 0, 7031);  // pc 17
  TEST_ASSERT_EQUAL(VmStatus::kEffect, start());
  TEST_ASSERT_EQUAL(101000, arg0());
  TEST_ASSERT_EQUAL(VmStatus::kEffect, g_vm.run(500, g_s));
  TEST_ASSERT_EQUAL(102000, arg0());
}

void test_vm_reporter_without_output_is_7031() {
  g_p.procs[0] = ProcEntry{2, 0, true, static_cast<uint16_t>(g_p.addWord("SIDE", 4))};
  g_p.n_procs = 1;
  emit(OpCode::kCall, 0, 0);
  emit(OpCode::kHalt);
  emit(OpCode::kFail, 0, 7031);  // pc 2
  TEST_ASSERT_EQUAL(VmStatus::kError, start());
  TEST_ASSERT_EQUAL_STRING("SIDE finished without giving back a value.", g_vm.error().msg);
}

void test_vm_sensors_and_random() {
  g_s.distance_milli = 25000;
  g_s.obstacle = true;
  emit(OpCode::kSensor, static_cast<uint8_t>(Sensor::kDistance));
  emit(OpCode::kSensor, static_cast<uint8_t>(Sensor::kObstacle));
  emit(OpCode::kPushN, 0, 6000);
  emit(OpCode::kCallB, static_cast<uint8_t>(Builtin::kRandom));
  fx(EffectKind::kArc, 3);
  TEST_ASSERT_EQUAL(VmStatus::kEffect, start());
  TEST_ASSERT_EQUAL(25000, g_vm.effect().args[0].n);
  TEST_ASSERT_EQUAL(VTag::kBool, g_vm.effect().args[1].tag);
  const int32_t r = g_vm.effect().args[2].n;
  TEST_ASSERT_TRUE(r >= 0 && r <= 5000 && r % 1000 == 0);
  TEST_ASSERT_TRUE(g_vm.sensorRead());
  start();
  TEST_ASSERT_EQUAL(r, g_vm.effect().args[2].n);  // same seed, same number
}

void test_vm_step_mode_pauses_at_each_new_line() {
  emit(OpCode::kLine, 0, 1);
  emit(OpCode::kLine, 0, 1);
  emit(OpCode::kLine, 0, 2);
  emit(OpCode::kHalt);
  g_vm.load(g_p, 1);
  g_vm.setStepping(true);
  TEST_ASSERT_EQUAL(VmStatus::kLine, g_vm.run(500, g_s));
  TEST_ASSERT_EQUAL(1, g_vm.line());
  TEST_ASSERT_EQUAL(VmStatus::kLine, g_vm.run(500, g_s));
  TEST_ASSERT_EQUAL(2, g_vm.line());
  TEST_ASSERT_EQUAL(VmStatus::kHalt, g_vm.run(500, g_s));
  g_vm.setStepping(false);
}

void test_vm_host_fail_ends_the_run() {
  fx(EffectKind::kForward, 0);
  emit(OpCode::kHalt);
  start();
  const DiagArg a[] = {{"word", "FORWARD"}, {"what", "a distance"}, {"min", "1"}, {"max", "500"}, {"unit", "cm"}};
  g_vm.fail(7025, a, 5);
  TEST_ASSERT_EQUAL(VmStatus::kError, g_vm.run(500, g_s));
  TEST_ASSERT_EQUAL_STRING("FORWARD needs a distance from 1 to 500 cm.", g_vm.error().msg);
}

void test_value_text_nested_lists() {
  const int16_t w = g_p.addWord("well", 4);
  const Value inner[] = {{VTag::kNum, -2500}};
  const int16_t li = g_p.addList(inner, 1);
  const Value outer[] = {{VTag::kWord, w}, {VTag::kList, li}, {VTag::kBool, 1}};
  const int16_t lo = g_p.addList(outer, 3);
  char out[64];
  valueText(g_p, Value{VTag::kList, lo}, true, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("[well [-2.5] TRUE]", out);
  valueText(g_p, Value{VTag::kList, lo}, false, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("well [-2.5] TRUE", out);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_vm_arithmetic_and_effect);
  RUN_TEST(test_vm_division_rounding_and_negatives);
  RUN_TEST(test_vm_divide_by_zero_and_too_big_have_the_line);
  RUN_TEST(test_vm_type_errors_name_the_word_and_value);
  RUN_TEST(test_vm_equality_of_words_and_lists);
  RUN_TEST(test_vm_repeat_rounds_and_repcount);
  RUN_TEST(test_vm_repeat_count_out_of_range);
  RUN_TEST(test_vm_empty_forever_yields_at_the_budget);
  RUN_TEST(test_vm_call_with_input_and_output);
  RUN_TEST(test_vm_tail_calls_use_one_frame);
  RUN_TEST(test_vm_deep_calls_are_7011);
  RUN_TEST(test_vm_output_inside_a_loop_pops_the_loop);
  RUN_TEST(test_vm_reporter_without_output_is_7031);
  RUN_TEST(test_vm_sensors_and_random);
  RUN_TEST(test_vm_step_mode_pauses_at_each_new_line);
  RUN_TEST(test_vm_host_fail_ends_the_run);
  RUN_TEST(test_value_text_nested_lists);
  return UNITY_END();
}
