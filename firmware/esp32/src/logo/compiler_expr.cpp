// Compiler: values (LLD §7) - numbers, words, lists, inputs, operators, reporters, calls of your own words.
#include <string.h>

#include "logo/compiler.h"
#include "logo/words.h"

namespace cq {
namespace logo {

bool Compiler::leaf(const Work& w) {
  const Node* n = w.n;
  const Token& t = m_toks.t[n->tok];
  int16_t c = 0;
  switch (n->kind) {  // VARIANT POINT: value leaves
    case NodeKind::kNumber: emit(OpCode::kPushN, 0, n->milli); return true;
    case NodeKind::kBool: emit(OpCode::kPushB, 0, n->milli); return true;
    case NodeKind::kInput: emit(OpCode::kArg, static_cast<uint8_t>(inputIndex(w.proc, n))); return true;
    case NodeKind::kQuoted: c = wordConst(t, 1); break;  // without its "
    case NodeKind::kPreset: c = wordConst(t, 0); break;
    case NodeKind::kList: c = listConst(n); break;
    default: full(n, "a word of a later version"); return true;  // the checker refuses these first (7030)
  }
  if (c < 0) full(n, "too many words and lists");
  else emit(OpCode::kPushC, 0, c);
  return true;
}

bool Compiler::stepOperator(Work& w) {
  if (child(w)) return false;
  emit(OpCode::kOp, static_cast<uint8_t>(w.n->op));
  return true;
}

// Your word: a statement gets its LINE; the inputs; CALL, or TCALL in tail position (grammar.md §8).
bool Compiler::stepProcCall(Work& w) {
  if (w.step == 0 && w.stmt) line(w.n);
  w.step = 1;
  if (child(w)) return false;
  emit(w.n->tail ? OpCode::kTCall : OpCode::kCall, static_cast<uint8_t>(w.n->count), w.n->ref);
  return true;
}

// A sensor (no inputs) or a built-in reporter (its inputs, then CALLB).
bool Compiler::stepReporter(Work& w, WordRole r, uint8_t arg) {
  if (r == WordRole::kSensor) {
    emit(OpCode::kSensor, arg);
    return true;
  }
  if (child(w)) return false;
  emit(OpCode::kCallB, arg);
  return true;
}

int16_t Compiler::wordConst(const Token& t, uint16_t skip) {
  return m_out->addWord(m_src + t.start + skip, static_cast<uint16_t>(t.len - skip));
}

bool Compiler::listItem(const Node* item, Value& out) {
  if (item->kind == NodeKind::kNumber) {
    out = Value{VTag::kNum, item->milli};
    return true;
  }
  const int16_t c = wordConst(m_toks.t[item->tok], 0);
  out = Value{VTag::kWord, c};
  return c >= 0;
}

// A list written in the program → a list constant (nested lists first). -1 when it does not fit.
int16_t Compiler::listConst(const Node* list) {
  struct Pos {
    const Node* next;
    uint16_t base;
  };
  constexpr uint8_t kDepth = 16;
  constexpr uint16_t kItems = 128;
  Pos st[kDepth];
  Value tmp[kItems];
  uint8_t ns = 0;
  uint16_t nv = 0;
  st[ns++] = Pos{list->first, 0};
  while (ns > 0) {
    Pos& top = st[ns - 1];
    if (top.next == nullptr) {
      const int16_t c = m_out->addList(tmp + top.base, static_cast<uint16_t>(nv - top.base));
      nv = top.base;
      if (--ns == 0 || c < 0) return c;
      tmp[nv++] = Value{VTag::kList, c};
      continue;
    }
    const Node* item = top.next;
    top.next = item->next;
    if (item->kind == NodeKind::kList) {
      if (ns >= kDepth) return -1;
      st[ns++] = Pos{item->first, nv};
    } else if (nv >= kItems || !listItem(item, tmp[nv++])) {
      return -1;
    }
  }
  return -1;
}

int8_t Compiler::inputIndex(int16_t proc, const Node* n) const {
  const Token& t = m_toks.t[n->tok];
  const ProcInfo& p = m_parser.proc(static_cast<uint8_t>(proc));
  for (uint8_t k = 0; k < p.n_inputs; ++k) {
    const Token& in = m_toks.t[p.first_input_tok + k];
    if (in.len == t.len && editDistance(m_src + in.start, in.len, m_src + t.start, t.len) == 0) {
      return static_cast<int8_t>(k);
    }
  }
  return 0;  // the checker refuses an unknown input first (7009)
}

}  // namespace logo
}  // namespace cq
