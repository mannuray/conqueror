// The Vm's instructions, one handler each in a table indexed by OpCode (internal to src/logo).
#pragma once

#include "logo/vm.h"

namespace cq {
namespace logo {

struct VmOps {
  static void run(Vm& vm, const Instr& in);

  static void halt(Vm& vm, const Instr& in);
  static void line(Vm& vm, const Instr& in);
  static void pushN(Vm& vm, const Instr& in);
  static void pushB(Vm& vm, const Instr& in);
  static void pushC(Vm& vm, const Instr& in);
  static void arg(Vm& vm, const Instr& in);
  static void op(Vm& vm, const Instr& in);
  static void sensor(Vm& vm, const Instr& in);
  static void callB(Vm& vm, const Instr& in);
  static void jmp(Vm& vm, const Instr& in);
  static void jz(Vm& vm, const Instr& in);
  static void jnz(Vm& vm, const Instr& in);
  static void loopInit(Vm& vm, const Instr& in);
  static void loopNext(Vm& vm, const Instr& in);
  static void call(Vm& vm, const Instr& in);
  static void tcall(Vm& vm, const Instr& in);
  static void ret(Vm& vm, const Instr& in);
  static void retV(Vm& vm, const Instr& in);
  static void effect(Vm& vm, const Instr& in);
  static void stop(Vm& vm, const Instr& in);
  static void fail(Vm& vm, const Instr& in);
  static void untilOver(Vm& vm, const Instr& in);
  static void pop(Vm& vm, const Instr& in);

  // helpers
  static bool popNumber(Vm& vm, const char* word, int32_t& out);
  static bool popBool(Vm& vm, const char* word, bool& out);
  static void binary(Vm& vm, Op o);
  static void equality(Vm& vm, Op o);
  static void logic(Vm& vm, Op o);
  static void unary(Vm& vm, Op o);
  static void builtin1(Vm& vm, Builtin b);
  static void calc(Vm& vm, Op o, int32_t a, int32_t b);
  static void random(Vm& vm);
};

}  // namespace logo
}  // namespace cq
