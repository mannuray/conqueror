// Compiler: statements (LLD §7) - effects; each statement starts with LINE (events, step mode, the line of a
// run-time error). Control words (REPEAT, IF … OUTPUT) are compiled from Plan N2 Task 4.
#include "logo/compiler.h"

namespace cq {
namespace logo {

bool Compiler::stepCall(Work& w) {
  uint8_t arg = 0;
  const WordRole r = role(w.n->ref, arg);
  if (r == WordRole::kEffect) return stepEffect(w, arg);
  if (r == WordRole::kSensor || r == WordRole::kBuiltin) return stepReporter(w, r, arg);
  full(w.n, "a word of a later version");
  return true;
}

// LINE, the inputs, EFFECT.
bool Compiler::stepEffect(Work& w, uint8_t kind) {
  if (w.step == 0) line(w.n);
  w.step = 1;
  if (child(w)) return false;
  emit(OpCode::kEffect, kind, w.n->count);
  return true;
}

}  // namespace logo
}  // namespace cq
