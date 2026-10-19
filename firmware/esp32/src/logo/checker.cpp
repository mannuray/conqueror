#include "logo/checker.h"

#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"
#include "logo/notes.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace {

void fmtMilli(int32_t m, char* out, size_t cap) {
  if (m % 1000 == 0) snprintf(out, cap, "%ld", static_cast<long>(m / 1000));
  else snprintf(out, cap, "%.3g", m / 1000.0);
}

bool templateInput(gen::InType t) { return t == gen::InType::kTemplate || t == gen::InType::kTemplateBlock; }

bool is(const char* a, const char* b) { return strcmp(a, b) == 0; }

// The statements that can be last in a procedure body (grammar.md §8).
struct TailStack {
  static constexpr uint8_t kCap = 64;
  Node* n[kCap];
  uint8_t ns = 0;
  void push(Node* x) {
    if (x != nullptr && ns < kCap) n[ns++] = x;
  }
};

// The last statements of IF / IFELSE blocks, and an OUTPUT of a call to our own word, are in tail position too.
void tailsOf(const Node* call, TailStack& st) {
  const char* name = gen::kWords[call->ref].name;
  if (is(name, "IF") && call->count == 2) {
    st.push(call->last->last);
  } else if (is(name, "IFELSE") && call->count == 3) {
    st.push(call->first->next->last);
    st.push(call->last->last);
  } else if (is(name, "OUTPUT") && call->first != nullptr && call->first->kind == NodeKind::kProcCall) {
    st.push(call->first);
  }
}

}  // namespace

void Checker::error(uint16_t code, uint16_t tok, const DiagArg* a, uint8_t n) {
  m_d.add(code, m_toks.t[tok].line, m_toks.t[tok].col, a, n);
}

void Checker::visitInput(const Node* n, int16_t def) {
  const Token& t = m_toks.t[n->tok];
  if (def >= 0) {
    const ProcInfo& p = m_p.proc(static_cast<uint8_t>(def));
    for (uint8_t k = 0; k < p.n_inputs; ++k) {
      const Token& in = m_toks.t[p.first_input_tok + k];
      if (in.len == t.len && editDistance(m_src + in.start, in.len, m_src + t.start, t.len) == 0) return;
    }
  }
  char name[40], sug[40] = "";
  snprintf(name, sizeof name, "%.*s", static_cast<int>(t.len - 1), m_src + t.start + 1);
  if (def >= 0) {
    const ProcInfo& p = m_p.proc(static_cast<uint8_t>(def));
    uint8_t best = 3;
    for (uint8_t k = 0; k < p.n_inputs; ++k) {
      const Token& in = m_toks.t[p.first_input_tok + k];
      const uint8_t d = editDistance(m_src + in.start + 1, static_cast<uint16_t>(in.len - 1), name,
                                     static_cast<uint16_t>(strlen(name)));
      if (d < best) { best = d; snprintf(sug, sizeof sug, "%.*s", static_cast<int>(in.len - 1), m_src + in.start + 1); }
    }
    if (best > 2) sug[0] = '\0';
  }
  const DiagArg a[] = {{"name", name}, {"suggestion", sug}};
  error(7009, n->tok, a, 2);
}

// 7020 (AI word), 7021 (needs encoders), then 7030 (a level-3 word reserved in v1): the first that applies.
void Checker::needsAndLevel(const gen::WordSpec& w, uint16_t tok, const Caps& caps) {
  const DiagArg wa[] = {{"word", w.name}};
  if ((w.needs & gen::kNeedAi) != 0 && !caps.ai) {
    error(7020, tok, wa, 1);
  } else if ((w.needs & gen::kNeedEncoders) != 0 && !caps.encoders) {
    error(7021, tok, wa, 1);
  } else if (w.level == 3 && !is(w.name, "OUTPUT") && caps.version == 1) {
    error(7030, tok, wa, 1);
  }
}

void Checker::visitCall(Node* n, int16_t def, const Caps& caps, CheckResult& r) {
  const gen::WordSpec& w = gen::kWords[n->ref];
  needsAndLevel(w, n->tok, caps);
  if (is(w.name, "OUTPUT") && def < 0) error(7026, n->tok);
  if (is(w.name, "BLOCKED?")) r.handles_obstacles = true;
  if (is(w.name, "PLAY") && n->first != nullptr && n->first->kind == NodeKind::kList) checkNotes(n->first);
  uint8_t i = 0;
  for (const Node* c = n->first; c != nullptr && i < w.n_inputs; c = c->next, ++i) checkRange(n, w, i, c);
}

// 7025: a written number outside the input's range; an ARC radius below it is 7022.
void Checker::checkRange(const Node* call, const gen::WordSpec& w, uint8_t i, const Node* c) {
  const gen::InputSpec& in = w.inputs[i];
  if (!in.has_range || c->kind != NodeKind::kNumber) return;
  if (c->milli >= in.min_milli && c->milli <= in.max_milli) return;
  if (is(w.name, "ARC") && is(in.name, "radius") && c->milli < in.min_milli) {
    arcTooTight(call, c);
    return;
  }
  char mn[16], mx[16];
  fmtMilli(in.min_milli, mn, sizeof mn);
  fmtMilli(in.max_milli, mx, sizeof mx);
  const DiagArg a[] = {{"word", w.name}, {"what", in.what}, {"min", mn}, {"max", mx}, {"unit", in.unit}};
  error(7025, c->tok, a, 5);
}

void Checker::arcTooTight(const Node* call, const Node* radius) {
  char word[24], ang[16], v[16];
  fmtMilli(radius->milli, v, sizeof v);
  tokenText(m_src, m_toks.t[call->first->tok], ang, sizeof ang);
  snprintf(word, sizeof word, "ARC %s", ang);
  fmtMilli(call->first->next->milli, ang, sizeof ang);
  const DiagArg a[] = {{"word", word}, {"angle", ang}, {"radius", v}};
  error(7022, radius->tok, a, 3);
}

// A PLAY list (words.yaml presets.notes): every item a note or the length of the note before it, 1 to 64 notes.
void Checker::checkNotes(const Node* list) {
  NoteReader rd;
  for (const Node* c = list->first; c != nullptr; c = c->next) {
    const Token& t = m_toks.t[c->tok];
    NoteReader::Err e = NoteReader::Err::kNotANote;
    if (c->kind == NodeKind::kWord) e = rd.word(m_src + t.start, t.len);
    if (c->kind == NodeKind::kNumber) e = rd.number(c->milli);
    if (e != NoteReader::Err::kNone) {
      notesError(static_cast<uint8_t>(e), c);
      return;
    }
  }
  if (rd.count() == 0) notesError(static_cast<uint8_t>(NoteReader::Err::kTooMany), list);
}

// 7029 for an item that is not a note; 7025 for a length or a note count out of range.
void Checker::notesError(uint8_t err, const Node* at) {
  if (static_cast<NoteReader::Err>(err) == NoteReader::Err::kNotANote) {
    char given[40];
    tokenText(m_src, m_toks.t[at->tok], given, sizeof given);
    const DiagArg a[] = {{"word", "PLAY"}, {"choices", "the notes C4 to B6 (C#4 for a sharp) and R"}, {"given", given}};
    error(7029, at->tok, a, 3);
    return;
  }
  const bool len = static_cast<NoteReader::Err>(err) == NoteReader::Err::kLength;
  const DiagArg a[] = {{"word", "PLAY"}, {"what", len ? "a length" : "a tune"}, {"min", len ? "0.25" : "1"},
                       {"max", len ? "16" : "64"}, {"unit", len ? "beats" : "notes"}};
  error(7025, at->tok, a, 5);
}

// 7019 (a warning): a :name inside a list is only a word there.
void Checker::listWarning(const Node* list) {
  for (const Node* c = list->first; c != nullptr; c = c->next) {
    if (c->kind == NodeKind::kWord && m_src[m_toks.t[c->tok].start] == ':') {
      m_d.add(7019, m_toks.t[c->tok].line, m_toks.t[c->tok].col, nullptr, 0, true);
      return;
    }
  }
}

void Checker::markTails(Node* body, CheckResult& r) {
  TailStack st;
  st.push(body->last);
  while (st.ns > 0) {
    Node* n = st.n[--st.ns];
    if (n->kind == NodeKind::kProcCall) {
      n->tail = true;
      if (n->ref < 64) r.tail_callees |= (1ULL << n->ref);
    } else if (n->kind == NodeKind::kCall) {
      tailsOf(n, st);
    }
  }
}

// Checks one node; returns the definition its children are inside, or kSkipChildren.
int16_t Checker::visit(Node* n, int16_t def, bool in_template, const Caps& caps, CheckResult& r) {
  switch (n->kind) {  // VARIANT POINT: checked node kinds
    case NodeKind::kDef: markTails(n->last, r); return n->ref;
    case NodeKind::kCall: visitCall(n, def, caps, r); return def;
    case NodeKind::kInput:
      if (def != kParamList) visitInput(n, def);
      return def;
    case NodeKind::kPlaceholder:
      if (!in_template) error(7006, n->tok);
      return def;
    case NodeKind::kList: listWarning(n); return kSkipChildren;  // data: nothing inside is looked up
    default: return def;
  }
}

// Checks the next child of the top entry and steps into it. False when the tree is too deep (7010).
bool Checker::descend(Item* st, uint16_t& ns, const Caps& caps, CheckResult& r) {
  Item& top = st[ns - 1];
  Node* c = top.next;
  const uint8_t i = top.i++;
  top.next = c->next;
  const Node* p = top.n;
  const bool call = p->kind == NodeKind::kCall;
  const bool tpl = top.in_template ||
                   (call && i < gen::kWords[p->ref].n_inputs && templateInput(gen::kWords[p->ref].inputs[i].type));
  const int16_t def = (p->kind == NodeKind::kDef && c->kind == NodeKind::kParams) ? kParamList : top.cdef;
  const int16_t cdef = visit(c, def, tpl, caps, r);
  if (cdef == kSkipChildren || c->first == nullptr) return true;
  if (ns >= kMaxWalk) {
    const DiagArg a[] = {{"what", "too deeply nested"}};
    error(7010, c->tok, a, 1);
    return false;
  }
  st[ns++] = {c, c->first, cdef, tpl, 0};
  return true;
}

CheckResult Checker::check(Node* program, const Caps& caps) {
  CheckResult r;
  if (program == nullptr) return r;
  Item st[kMaxWalk];
  uint16_t ns = 0;
  st[ns++] = {program, program->first, visit(program, -1, false, caps, r), false, 0};
  while (ns > 0) {
    if (st[ns - 1].next == nullptr) {
      --ns;
    } else if (!descend(st, ns, caps, r)) {
      break;
    }
  }
  r.ok = m_d.errorCount() == 0;
  return r;
}

}  // namespace logo
}  // namespace cq
