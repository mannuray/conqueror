#include "logo/vm_ops.h"

#include <stdio.h>

#include "logo/vm_calc.h"

namespace cq {
namespace logo {
namespace {

using OpFn = void (*)(Vm&, const Instr&);
constexpr OpFn kOps[] = {VmOps::halt,     VmOps::line,     VmOps::pushN, VmOps::pushB, VmOps::pushC, VmOps::arg,
                         VmOps::op,       VmOps::sensor,   VmOps::callB, VmOps::jmp,   VmOps::jz,    VmOps::jnz,
                         VmOps::loopInit, VmOps::loopNext, VmOps::call,  VmOps::tcall, VmOps::ret,   VmOps::retV,
                         VmOps::effect,   VmOps::stop,     VmOps::fail,  VmOps::untilOver, VmOps::pop};
static_assert(sizeof kOps / sizeof kOps[0] == static_cast<size_t>(OpCode::kCount), "one handler per OpCode");

// The word a condition belongs to (Instr::a of kJz / kJnz), for 7012.
constexpr const char* kCondWords[] = {"IF", "IFELSE", "WHILE", "WAIT UNTIL"};

Value num(int32_t n) { return Value{VTag::kNum, n}; }
Value boolean(bool b) { return Value{VTag::kBool, b ? 1 : 0}; }

}  // namespace

void VmOps::run(Vm& vm, const Instr& in) { kOps[static_cast<uint8_t>(in.op)](vm, in); }

void VmOps::halt(Vm& vm, const Instr&) { vm.m_done = vm.m_signal = VmStatus::kHalt; }

void VmOps::line(Vm& vm, const Instr& in) {
  const bool changed = static_cast<uint16_t>(in.n) != vm.m_line;
  vm.m_line = static_cast<uint16_t>(in.n);
  if (vm.m_stepping && changed) vm.signal(VmStatus::kLine);
}

void VmOps::pushN(Vm& vm, const Instr& in) { vm.push(num(in.n)); }
void VmOps::pushB(Vm& vm, const Instr& in) { vm.push(boolean(in.n != 0)); }
void VmOps::pushC(Vm& vm, const Instr& in) { vm.push(Value{vm.m_p->consts[in.n].tag, in.n}); }

void VmOps::arg(Vm& vm, const Instr& in) {
  const Vm::Frame& f = vm.m_frames[vm.m_nf - 1];
  vm.push(vm.m_stack[f.args_base + in.a]);
}

bool VmOps::popNumber(Vm& vm, const char* word, int32_t& out) {
  const Value v = vm.pop();
  if (v.tag != VTag::kNum) {
    vm.typeError(word, "a number", v);
    return false;
  }
  out = v.n;
  return true;
}

bool VmOps::popBool(Vm& vm, const char* word, bool& out) {
  const Value v = vm.pop();
  if (v.tag != VTag::kBool) {
    vm.typeError(word, "TRUE or FALSE", v);
    return false;
  }
  out = v.n != 0;
  return true;
}

void VmOps::calc(Vm& vm, Op o, int32_t a, int32_t b) {
  int32_t r = 0;
  const CalcErr e = arith(o, a, b, r);
  char line[8];
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(vm.m_line));
  const DiagArg la[] = {{"line", line}};
  if (e == CalcErr::kDivZero) return vm.error(7013, la, 1);
  if (e == CalcErr::kTooBig) return vm.error(7033, la, 1);
  vm.push(num(r));
}

void VmOps::binary(Vm& vm, Op o) {
  if (o == Op::kEq || o == Op::kNe) return equality(vm, o);
  if (o == Op::kAnd || o == Op::kOr) return logic(vm, o);
  int32_t a = 0, b = 0;
  if (!popNumber(vm, opSymbol(o), b) || !popNumber(vm, opSymbol(o), a)) return;
  if (o >= Op::kLt && o <= Op::kGe) return (void)vm.push(boolean(compare(o, a, b)));
  calc(vm, o, a, b);
}

// = and <> take any two values (words without regard to case, lists by their items).
void VmOps::equality(Vm& vm, Op o) {
  const Value b = vm.pop(), a = vm.pop();
  const bool same = sameValue(*vm.m_p, a, b);
  vm.push(boolean(o == Op::kEq ? same : !same));
}

void VmOps::logic(Vm& vm, Op o) {
  bool a = false, b = false;
  if (!popBool(vm, opSymbol(o), b) || !popBool(vm, opSymbol(o), a)) return;
  vm.push(boolean(o == Op::kAnd ? (a && b) : (a || b)));
}

void VmOps::unary(Vm& vm, Op o) {
  if (o == Op::kNot) {
    bool b = false;
    if (popBool(vm, "NOT", b)) vm.push(boolean(!b));
    return;
  }
  int32_t x = 0;
  if (popNumber(vm, "-", x)) vm.push(num(-x));
}

void VmOps::op(Vm& vm, const Instr& in) {
  const Op o = static_cast<Op>(in.a);
  if (o == Op::kNot || o == Op::kNeg) return unary(vm, o);
  binary(vm, o);
}

void VmOps::sensor(Vm& vm, const Instr& in) {
  const Sensors& s = *vm.m_sensors;
  const Sensor k = static_cast<Sensor>(in.a);
  vm.m_sensor_read = true;
  switch (k) {  // VARIANT POINT: sensors (words.yaml level 2 reporters)
    case Sensor::kDistance: vm.push(num(s.distance_milli)); return;
    case Sensor::kObstacle: vm.push(boolean(s.obstacle)); return;
    case Sensor::kBlocked: vm.push(boolean(s.blocked)); return;
    case Sensor::kHeading: vm.push(num(s.heading_milli)); return;
    case Sensor::kTilted: vm.push(boolean(s.tilted)); return;
    case Sensor::kBattery: vm.push(num(s.battery_milli)); return;
    case Sensor::kTimer: vm.push(num(s.timer_milli)); return;
  }
}

// RANDOM n: a whole number from 0 to n - 1 (xorshift32, seeded by the host).
void VmOps::random(Vm& vm) {
  int32_t n = 0;
  if (!popNumber(vm, "RANDOM", n)) return;
  vm.m_sensor_read = true;
  const int32_t whole = n / 1000;
  if (whole < 1 || whole > 1000000) {
    const DiagArg a[] = {{"word", "RANDOM"}, {"what", "a number"}, {"min", "1"}, {"max", "1000000"}, {"unit", ""}};
    return vm.error(7025, a, 5);
  }
  uint32_t x = vm.m_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  vm.m_rng = x;
  vm.push(num(static_cast<int32_t>(x % static_cast<uint32_t>(whole)) * 1000));
}

void VmOps::builtin1(Vm& vm, Builtin b) {
  int32_t x = 0;
  if (!popNumber(vm, b == Builtin::kAbs ? "ABS" : "ROUND", x)) return;
  vm.push(num(b == Builtin::kAbs ? (x < 0 ? -x : x) : roundWhole(x)));
}

void VmOps::callB(Vm& vm, const Instr& in) {
  const Builtin b = static_cast<Builtin>(in.a);
  switch (b) {  // VARIANT POINT: built-in reporters
    case Builtin::kRandom: return random(vm);
    case Builtin::kRepcount: vm.push(num(vm.round() * 1000)); return;
    case Builtin::kRemainder: {
      int32_t a = 0, d = 0, r = 0;
      if (!popNumber(vm, "REMAINDER", d) || !popNumber(vm, "REMAINDER", a)) return;
      if (remainder(a, d, r) == CalcErr::kNone) return (void)vm.push(num(r));
      char line[8];
      snprintf(line, sizeof line, "%u", static_cast<unsigned>(vm.m_line));
      const DiagArg la[] = {{"line", line}};
      return vm.error(7013, la, 1);
    }
    default: return builtin1(vm, b);
  }
}

void VmOps::jmp(Vm& vm, const Instr& in) { vm.m_pc = static_cast<uint16_t>(in.n); }

void VmOps::jz(Vm& vm, const Instr& in) {
  bool b = false;
  if (popBool(vm, kCondWords[in.a], b) && !b) vm.m_pc = static_cast<uint16_t>(in.n);
}

void VmOps::jnz(Vm& vm, const Instr& in) {
  bool b = false;
  if (popBool(vm, kCondWords[in.a], b) && b) vm.m_pc = static_cast<uint16_t>(in.n);
}

// REPEAT (a = 0: pops the count, whole part, 0..10000) or FOREVER (a = 1).
void VmOps::loopInit(Vm& vm, const Instr& in) {
  int32_t count = -1;
  if (in.a == 0) {
    if (!popNumber(vm, "REPEAT", count)) return;
    if (count < 0 || count > 10000000) {
      const DiagArg a[] = {{"word", "REPEAT"}, {"what", "how many times"}, {"min", "0"}, {"max", "10000"}, {"unit", ""}};
      return vm.error(7025, a, 5);
    }
    count /= 1000;
  }
  if (vm.m_nl >= Vm::kMaxLoops) return vm.error(7014);
  vm.m_loops[vm.m_nl++] = Vm::Loop{count, 0};
}

// The next round of the innermost loop, or past its end (n) when it is done.
void VmOps::loopNext(Vm& vm, const Instr& in) {
  Vm::Loop& l = vm.m_loops[vm.m_nl - 1];
  if (l.count < 0 || l.round < l.count) {
    ++l.round;
    return;
  }
  --vm.m_nl;
  vm.m_pc = static_cast<uint16_t>(in.n);
}

// CALL p (a = number of inputs, already on the stack).
void VmOps::call(Vm& vm, const Instr& in) {
  if (vm.m_nf >= Vm::kMaxFrames) return vm.error(7011);
  vm.m_frames[vm.m_nf++] = Vm::Frame{vm.m_pc, static_cast<uint16_t>(vm.m_sp - in.a), static_cast<int8_t>(in.n), vm.m_nl};
  vm.m_pc = vm.m_p->procs[in.n].entry;
}

// A call in tail position (grammar.md §8): the new inputs replace the current frame's, no new frame.
void VmOps::tcall(Vm& vm, const Instr& in) {
  Vm::Frame& f = vm.m_frames[vm.m_nf - 1];
  const uint16_t from = static_cast<uint16_t>(vm.m_sp - in.a);
  for (uint8_t i = 0; i < in.a; ++i) vm.m_stack[f.args_base + i] = vm.m_stack[from + i];
  vm.m_sp = static_cast<uint16_t>(f.args_base + in.a);
  vm.m_nl = f.loop_base;
  f.proc = static_cast<int8_t>(in.n);
  vm.m_pc = vm.m_p->procs[in.n].entry;
}

void VmOps::ret(Vm& vm, const Instr&) { vm.leaveFrame(); }

void VmOps::retV(Vm& vm, const Instr&) {
  const Value v = vm.pop();
  vm.leaveFrame();
  vm.push(v);
}

// EFFECT (a = EffectKind, n = number of inputs on the stack): the host does it, then calls run() again.
void VmOps::effect(Vm& vm, const Instr& in) {
  Effect& e = vm.m_effect;
  e.kind = static_cast<EffectKind>(in.a);
  e.n = static_cast<uint8_t>(in.n);
  e.line = vm.m_line;
  for (uint8_t i = e.n; i > 0; --i) e.args[i - 1] = vm.pop();
  vm.signal(VmStatus::kEffect);
}

void VmOps::stop(Vm& vm, const Instr&) { vm.m_done = vm.m_signal = VmStatus::kStop; }

// FAIL (n = error code): 7031, a word that gives back a value reached END without OUTPUT.
void VmOps::fail(Vm& vm, const Instr& in) {
  const Vm::Frame& f = vm.m_frames[vm.m_nf - 1];
  uint16_t len = 0;
  const char* name = vm.m_p->wordText(vm.m_p->procs[f.proc].name, len);
  char word[40];
  snprintf(word, sizeof word, "%.*s", static_cast<int>(len), name);
  const DiagArg a[] = {{"word", word}};
  vm.error(static_cast<uint16_t>(in.n), a, 1);
}

// UNTIL_OVER: the WAIT UNTIL whose start time (TIMER) is on top of the stack has waited its 60 s (it stays there).
void VmOps::untilOver(Vm& vm, const Instr&) {
  const int32_t start = vm.m_stack[vm.m_sp - 1].n;
  vm.push(boolean(vm.m_sensors->timer_milli - start >= static_cast<int32_t>(gen::kWaitS) * 1000));
}

void VmOps::pop(Vm& vm, const Instr&) { vm.pop(); }

}  // namespace logo
}  // namespace cq
