// Compiler: statements (LLD §7) - effects, REPEAT / FOREVER / IF / IFELSE / WHILE / WAIT UNTIL, STOP, OUTPUT.
// Each statement starts with LINE (events, step mode, the line of a run-time error).
#include "logo/compiler.h"

namespace cq {
namespace logo {
namespace {

// Instr::a of kJz / kJnz: the word a condition belongs to (Vm 7012 messages).
constexpr uint8_t kCondIf = 0, kCondIfElse = 1, kCondWhile = 2, kCondUntil = 3;

}  // namespace

bool Compiler::stepCall(Work& w) {
  using StepFn = bool (Compiler::*)(Work&);
  static constexpr StepFn kControl[] = {&Compiler::stepRepeat, &Compiler::stepForever,   &Compiler::stepIf,
                                        &Compiler::stepIfElse, &Compiler::stepWhile,     &Compiler::stepWaitUntil,
                                        &Compiler::stepStop,   &Compiler::stepOutput};
  static_assert(static_cast<uint8_t>(WordRole::kOutput) - static_cast<uint8_t>(WordRole::kRepeat) + 1 ==
                    sizeof kControl / sizeof kControl[0],
                "one step per control word");
  uint8_t arg = 0;
  const WordRole r = role(w.n->ref, arg);
  if (r == WordRole::kEffect) return stepEffect(w, arg);
  if (r == WordRole::kSensor || r == WordRole::kBuiltin) return stepReporter(w, r, arg);
  if (r == WordRole::kNone) {
    full(w.n, "a word of a later version");  // the checker refuses these first (7030)
    return true;
  }
  return (this->*kControl[static_cast<uint8_t>(r) - static_cast<uint8_t>(WordRole::kRepeat)])(w);
}

// LINE, the inputs, EFFECT.
bool Compiler::stepEffect(Work& w, uint8_t kind) {
  if (w.step == 0) line(w.n);
  w.step = 1;
  if (child(w)) return false;
  emit(OpCode::kEffect, kind, w.n->count);
  return true;
}

// LINE count LOOP_INIT; L: LOOP_NEXT end; body; JMP L; end:
bool Compiler::stepRepeat(Work& w) {
  if (w.step == 0) {
    line(w.n);
    w.step = 1;
    return !child(w);
  }
  if (w.step == 1) {
    emit(OpCode::kLoopInit, 0);
    w.p1 = emit(OpCode::kLoopNext);
    w.step = 2;
    return !child(w);
  }
  emit(OpCode::kJmp, 0, w.p1);
  patch(w.p1);
  return true;
}

// LINE LOOP_INIT(forever); L: LOOP_NEXT; body; JMP L
bool Compiler::stepForever(Work& w) {
  if (w.step == 0) {
    line(w.n);
    emit(OpCode::kLoopInit, 1);
    w.p1 = emit(OpCode::kLoopNext);
    w.step = 1;
    return !child(w);
  }
  emit(OpCode::kJmp, 0, w.p1);
  patch(w.p1);
  return true;
}

// LINE test JZ end; block; end:
bool Compiler::stepIf(Work& w) {
  if (w.step == 0) {
    line(w.n);
    w.step = 1;
    return !child(w);
  }
  if (w.step == 1) {
    w.p1 = emit(OpCode::kJz, kCondIf);
    w.step = 2;
    return !child(w);
  }
  patch(w.p1);
  return true;
}

// LINE test JZ no; yes; JMP end; no: no; end:
bool Compiler::stepIfElse(Work& w) {
  switch (w.step) {
    case 0:
      line(w.n);
      break;
    case 1:
      w.p1 = emit(OpCode::kJz, kCondIfElse);
      break;
    case 2:
      w.p2 = emit(OpCode::kJmp);
      patch(w.p1);
      break;
    default:
      patch(w.p2);
      return true;
  }
  ++w.step;
  return !child(w);
}

// L: LINE test JZ end; body; JMP L; end:
bool Compiler::stepWhile(Work& w) {
  if (w.step == 0) {
    w.p2 = m_out->n_code;
    line(w.n);
    w.step = 1;
    return !child(w);
  }
  if (w.step == 1) {
    w.p1 = emit(OpCode::kJz, kCondWhile);
    w.step = 2;
    return !child(w);
  }
  emit(OpCode::kJmp, 0, w.p2);
  patch(w.p1);
  return true;
}

// LINE EFFECT(start, 60 s); L: test JNZ end; EFFECT(tick); SENSOR(over); JZ L; end:
bool Compiler::stepWaitUntil(Work& w) {
  if (w.step == 0) {
    line(w.n);
    emit(OpCode::kEffect, static_cast<uint8_t>(EffectKind::kUntilStart), 0);
    w.p2 = m_out->n_code;
    w.step = 1;
    return !child(w);
  }
  w.p1 = emit(OpCode::kJnz, kCondUntil);
  emit(OpCode::kEffect, static_cast<uint8_t>(EffectKind::kTick), 0);
  emit(OpCode::kSensor, static_cast<uint8_t>(Sensor::kUntilOver));
  emit(OpCode::kJz, kCondUntil, w.p2);
  patch(w.p1);
  return true;
}

bool Compiler::stepStop(Work& w) {
  line(w.n);
  emit(OpCode::kStop);
  return true;
}

// LINE value RETV - or, for OUTPUT of a call of your word in tail position, that call's TCALL only.
bool Compiler::stepOutput(Work& w) {
  if (w.step == 0) {
    line(w.n);
    const Node* v = w.n->first;
    w.step = (v->kind == NodeKind::kProcCall && v->tail) ? 2 : 1;
    return !child(w);
  }
  if (w.step == 1) emit(OpCode::kRetV);
  return true;
}

}  // namespace logo
}  // namespace cq
