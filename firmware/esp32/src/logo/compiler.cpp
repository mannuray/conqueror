#include "logo/compiler.h"

#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

struct WordUse {
  const char* name;
  WordRole role;
  uint8_t arg;  // EffectKind / Sensor / Builtin
};
constexpr WordUse fx(const char* n, EffectKind k) { return {n, WordRole::kEffect, static_cast<uint8_t>(k)}; }
constexpr WordUse sensor(const char* n, Sensor s) { return {n, WordRole::kSensor, static_cast<uint8_t>(s)}; }
constexpr WordUse builtin(const char* n, Builtin b) { return {n, WordRole::kBuiltin, static_cast<uint8_t>(b)}; }
constexpr WordUse kUses[] = {
    fx("FORWARD", EffectKind::kForward), fx("BACK", EffectKind::kBack), fx("RIGHT", EffectKind::kRight),
    fx("LEFT", EffectKind::kLeft), fx("SPEED", EffectKind::kSpeed), fx("WAIT", EffectKind::kWait),
    fx("LED", EffectKind::kLed), fx("BEEP", EffectKind::kBeep), fx("SOUND", EffectKind::kSound),
    fx("LOOK", EffectKind::kLook), fx("HOME", EffectKind::kHome), fx("ARC", EffectKind::kArc),
    fx("PIVOT", EffectKind::kPivot), fx("PLAY", EffectKind::kPlay), fx("SAY", EffectKind::kSay),
    {"REPEAT", WordRole::kRepeat, 0}, {"FOREVER", WordRole::kForever, 0}, {"IF", WordRole::kIf, 0},
    {"IFELSE", WordRole::kIfElse, 0}, {"WHILE", WordRole::kWhile, 0}, {"WAIT UNTIL", WordRole::kWaitUntil, 0},
    {"STOP", WordRole::kStop, 0}, {"OUTPUT", WordRole::kOutput, 0},
    sensor("DISTANCE", Sensor::kDistance), sensor("OBSTACLE?", Sensor::kObstacle),
    sensor("BLOCKED?", Sensor::kBlocked), sensor("HEADING", Sensor::kHeading), sensor("TILTED?", Sensor::kTilted),
    sensor("BATTERY", Sensor::kBattery), sensor("TIMER", Sensor::kTimer), builtin("RANDOM", Builtin::kRandom),
    builtin("REMAINDER", Builtin::kRemainder), builtin("ABS", Builtin::kAbs), builtin("ROUND", Builtin::kRound),
    builtin("REPCOUNT", Builtin::kRepcount),
};

}  // namespace

bool Compiler::compile(const Node* program, bool handles_obstacles, Program& out) {
  m_out = &out;
  out.clear();
  out.handles_obstacles = handles_obstacles;
  m_failed = false;
  m_ns = 0;
  for (uint8_t i = 0; i < m_parser.procCount(); ++i) {
    const ProcInfo& pi = m_parser.proc(i);
    const int16_t name = wordConst(m_toks.t[pi.name_tok], 0);
    out.procs[i] = ProcEntry{0, pi.n_inputs, pi.gives_value, static_cast<uint16_t>(name < 0 ? 0 : name)};
  }
  out.n_procs = m_parser.procCount();
  pushWork(program, -1);
  while (m_ns > 0 && !m_failed) {
    if (stepNode(m_st[m_ns - 1])) --m_ns;
  }
  return !m_failed;
}

bool Compiler::stepNode(Work& w) {
  switch (w.n->kind) {  // VARIANT POINT: node kinds the compiler knows
    case NodeKind::kProgram: return stepProgram(w);
    case NodeKind::kDef: return stepDef(w);
    case NodeKind::kBlock: return stepBlock(w);
    case NodeKind::kCall: return stepCall(w);
    case NodeKind::kProcCall: return stepProcCall(w);
    case NodeKind::kUnary:
    case NodeKind::kBinary: return stepOperator(w);
    default: return leaf(w);
  }
}

// The statements (definitions skipped), HALT, then each definition.
bool Compiler::stepProgram(Work& w) {
  if (w.step == 0) {
    while (w.next != nullptr && w.next->kind == NodeKind::kDef) w.next = w.next->next;
    if (child(w)) return false;
    emit(OpCode::kHalt);
    w.step = 1;
    w.next = w.n->first;
  }
  while (w.next != nullptr && w.next->kind != NodeKind::kDef) w.next = w.next->next;
  return !child(w);
}

// A word's body; then RET (a command) or FAIL 7031 (a word that should have given back a value).
bool Compiler::stepDef(Work& w) {
  if (w.step == 0) {
    m_out->procs[w.n->ref].entry = m_out->n_code;
    w.step = 1;
    pushWork(w.n->last, w.n->ref);
    return false;
  }
  if (m_out->procs[w.n->ref].gives_value) emit(OpCode::kFail, 0, 7031);
  else emit(OpCode::kRet);
  return true;
}

bool Compiler::stepBlock(Work& w) { return !child(w); }

bool Compiler::child(Work& w) {
  if (w.next == nullptr) return false;
  const Node* c = w.next;
  w.next = c->next;
  pushWork(c, w.n->kind == NodeKind::kDef ? w.n->ref : w.proc);
  if (!m_failed) m_st[m_ns - 1].stmt = w.n->kind == NodeKind::kBlock || w.n->kind == NodeKind::kProgram;
  return true;
}

void Compiler::pushWork(const Node* n, int16_t proc) {
  if (m_ns >= kMaxWork) return full(n, "too deeply nested");
  m_st[m_ns++] = Work{n, n->first, proc, 0, false, 0, 0};
}

uint16_t Compiler::emit(OpCode op, uint8_t a, int32_t n) {
  if (m_out->n_code >= Program::kMaxCode) {
    full(nullptr, "too many steps");
    return 0;
  }
  m_out->code[m_out->n_code] = Instr{op, a, n};
  return m_out->n_code++;
}

void Compiler::patch(uint16_t at) { m_out->code[at].n = m_out->n_code; }

void Compiler::line(const Node* n) { emit(OpCode::kLine, 0, m_toks.t[n->tok].line); }

// 7010: the program does not fit (at the node, or at the start when there is none).
void Compiler::full(const Node* n, const char* what) {
  if (m_failed) return;
  m_failed = true;
  const Token& t = m_toks.t[n != nullptr ? n->tok : 0];
  const DiagArg a[] = {{"what", what}};
  m_d.add(7010, t.line, t.col, a, 1);
}

WordRole Compiler::role(int16_t word, uint8_t& arg) const {
  const char* name = gen::kWords[word].name;
  for (const WordUse& u : kUses) {
    if (strcmp(u.name, name) != 0) continue;
    arg = u.arg;
    return u.role;
  }
  return WordRole::kNone;
}

}  // namespace logo
}  // namespace cq
