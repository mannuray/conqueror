// Parser pass 2, statements (grammar.md §3-§5, §9): sequences, definitions, calls and their inputs, note lists.
#include <stdio.h>

#include "logo/parser.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace {

bool isPresetType(gen::InType t) {
  return t == gen::InType::kSide || t == gen::InType::kColour || t == gen::InType::kLook || t == gen::InType::kSound;
}

}  // namespace

void Parser::stepSeq(Frame& f) {
  const Token& t = tok();
  if (t.kind == Tok::kEnd) { seqAtEof(f); return; }
  if (t.kind == Tok::kRBracket) { seqAtBracket(f); return; }
  if (t.kind != Tok::kName) { leftover(m_i); fail(true); return; }
  if (seqBlockWord(f, t) || seqSpecialWord(t)) return;
  seqWord(t);
}

void Parser::seqAtEof(const Frame& f) {
  if (f.close == Close::kEof) { --m_nf; return; }
  const bool def = f.close == Close::kEnd;
  unclosed(f.open_tok, def ? "TO" : "[", def ? "END" : "]");
  m_abort = true;
}

void Parser::seqAtBracket(const Frame& f) {
  if (f.close == Close::kBracket) { ++m_i; --m_nf; return; }
  char line[8];
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(tok().line));
  const DiagArg a[] = {{"char", "]"}, {"line", line}};
  error(7016, m_i, a, 2);
  ++m_i;
}

// END and TO. True when the word was one of them.
bool Parser::seqBlockWord(const Frame& f, const Token& t) {
  if (tokenIs(m_src, t, "END")) {
    if (f.close == Close::kEnd) { ++m_i; --m_nf; return true; }
    unknownWord(m_i);
    ++m_i;
    return true;
  }
  if (!tokenIs(m_src, t, "TO")) return false;
  if (m_nf == 1) parseDef();
  else { ++m_i; skipToEnd(); }  // pass 1 reported 7017
  return true;
}

// MAKE / RUN / FOR (7027, not in Tank Logo) and the two-word WAIT UNTIL. True when the word was one of them.
bool Parser::seqSpecialWord(const Token& t) {
  const uint16_t at = m_i;
  if (tokenIs(m_src, t, "MAKE") || tokenIs(m_src, t, "RUN") || tokenIs(m_src, t, "FOR")) {
    char nm[40];
    tokenText(m_src, t, nm, sizeof nm);
    const DiagArg a[] = {{"word", nm}, {"hint", "Values come in as inputs of your own words: TO NAME :input."}};
    error(7027, at, a, 2);
    fail(true);
    return true;
  }
  if (!tokenIs(m_src, t, "WAIT") || !tokenIs(m_src, tokAt(static_cast<uint16_t>(at + 1)), "UNTIL")) return false;
  m_i = static_cast<uint16_t>(m_i + 2);
  startCall(wordIndex("WAIT UNTIL"), at);
  return true;
}

// A command (built-in or our own) starts a call; a value here is left over; anything else is unknown.
void Parser::seqWord(const Token& t) {
  const uint16_t at = m_i;
  const int16_t w = findWord(m_src, t);
  const int16_t p = w >= 0 ? -1 : findProc(t);
  if (isCommandWord(t)) {
    ++m_i;
    if (w >= 0) startCall(w, at);
    else startProcCall(p, at);
    return;
  }
  if (w >= 0 || p >= 0) leftover(at);
  else unknownWord(at);
  fail(true);
}

void Parser::skipToEnd() {
  while (tok().kind != Tok::kEnd && !tokenIs(m_src, tok(), "END")) ++m_i;
  if (tok().kind != Tok::kEnd) ++m_i;
}

void Parser::parseDef() {
  int16_t p = -1;
  for (uint8_t i = 0; i < m_n_procs; ++i) {
    if (m_procs[i].to_tok == m_i) p = i;
  }
  if (p < 0 || !m_procs[p].valid) { ++m_i; skipToEnd(); return; }  // pass 1 reported it
  const ProcInfo& info = m_procs[p];
  Node* d = make(NodeKind::kDef, m_i);
  Node* params = make(NodeKind::kParams, m_i);
  Node* body = make(NodeKind::kBlock, m_i);
  if (d == nullptr || params == nullptr || body == nullptr) return;
  d->ref = p;
  for (uint8_t k = 0; k < info.n_inputs; ++k) {
    Node* in = make(NodeKind::kInput, static_cast<uint16_t>(info.first_input_tok + k));
    if (in == nullptr) return;
    params->add(in);
  }
  d->add(params);
  d->add(body);
  m_root->add(d);
  const uint16_t to = m_i;
  m_i = static_cast<uint16_t>(info.first_input_tok + info.n_inputs);
  pushSeq(Close::kEnd, body, to);
}

void Parser::stepCall(Frame& f) {
  const uint8_t n_inputs = f.node->kind == NodeKind::kCall ? gen::kWords[f.node->ref].n_inputs
                                                            : m_procs[f.node->ref].n_inputs;
  if (f.next_input >= n_inputs) {
    Node* n = f.node;
    --m_nf;
    deliver(n);
    return;
  }
  if (inputEnds(f)) { missingInput(f); fail(false); return; }
  readInput(f);
}

// The next input is missing: the text, a block or a bracket ends, END, or a command on a later line.
bool Parser::inputEnds(const Frame& f) const {
  const Token& t = tok();
  return t.kind == Tok::kEnd || t.kind == Tok::kRBracket || t.kind == Tok::kRParen || tokenIs(m_src, t, "END") ||
         (isCommandWord(t) && t.line > tokAt(f.open_tok).line);
}

void Parser::readInput(Frame& f) {
  gen::InType type;
  const char *what, *example, *name;
  inputSpec(f, type, what, example, name);
  const uint16_t at = m_i;
  if (isPresetType(type)) { callPreset(f, type, name); return; }
  switch (type) {  // VARIANT POINT: input types (grammar.md §5)
    case gen::InType::kBlock:
    case gen::InType::kTemplateBlock: callBlock(f, name, example); return;
    case gen::InType::kTemplate:
      if (tok().kind == Tok::kLBracket) { ++m_i; pushExpr(true, Close::kBracket, true, at); }
      else pushExpr(false, Close::kNone, false, at);
      return;
    case gen::InType::kNotes: callNotes(f); return;
    case gen::InType::kCondition: pushExpr(true, Close::kNone, false, at); return;
    default: pushExpr(false, Close::kNone, false, at); return;
  }
}

void Parser::callBlock(Frame& f, const char* name, const char* example) {
  const uint16_t at = m_i;
  if (tok().kind != Tok::kLBracket) {
    char nm[40];
    snprintf(nm, sizeof nm, "%s", name);
    const DiagArg a[] = {{"word", nm}, {"example", example}};
    error(7023, at, a, 2);
    minusHint(at);
    fail(true);
    return;
  }
  Node* b = make(NodeKind::kBlock, at);
  if (b == nullptr) return;
  f.node->add(b);
  ++f.next_input;
  ++m_i;
  pushSeq(Close::kBracket, b, at);
}

void Parser::callNotes(Frame& f) {
  if (tok().kind != Tok::kLBracket) { missingInput(f); fail(true); return; }
  openList(m_i);
}

// Side, colour, look and sound inputs take a preset name; colour, look and sound also take a value (grammar.md §5).
void Parser::callPreset(Frame& f, gen::InType type, const char* name) {
  const Token& t = tok();
  const uint16_t at = m_i;
  if (t.kind == Tok::kName && isPresetOf(m_src, t, type)) {
    Node* p = make(NodeKind::kPreset, at);
    if (p == nullptr) return;
    p->ref = static_cast<int16_t>(type);
    ++m_i;
    f.node->add(p);
    ++f.next_input;
    return;
  }
  if (type == gen::InType::kSide || (t.kind == Tok::kName && !givesValue(t))) {
    char nm[40], given[40], choices[96];
    snprintf(nm, sizeof nm, "%s", name);
    tokenText(m_src, t, given, sizeof given);
    presetChoices(type, choices, sizeof choices);
    const DiagArg a[] = {{"word", nm}, {"choices", choices}, {"given", given}};
    error(7029, at, a, 3);
    fail(true);
    return;
  }
  pushExpr(false, Close::kNone, false, at);
}

// At [: a new list that is delivered as a value when its ] closes.
void Parser::openList(uint16_t at) {
  Node* l = make(NodeKind::kList, at);
  if (l == nullptr) return;
  ++m_i;
  pushList(l, at, false);
}

void Parser::stepList(Frame& f) {
  switch (tok().kind) {
    case Tok::kEnd:
      unclosed(f.open_tok, "[", "]");
      m_abort = true;
      return;
    case Tok::kRBracket: closeList(f); return;
    case Tok::kLBracket: {
      const uint16_t at = m_i;
      Node* l = make(NodeKind::kList, at);
      if (l == nullptr) return;
      f.node->add(l);
      ++m_i;
      pushList(l, at, true);
      return;
    }
    default: listItem(f); return;
  }
}

void Parser::closeList(const Frame& f) {
  ++m_i;
  Node* n = f.node;
  const bool attached = f.attached;
  --m_nf;
  if (!attached) deliver(n);
}

// A number (a - straight before it makes it negative) or a word, kept as written.
void Parser::listItem(Frame& f) {
  const Token& t = tok();
  const uint16_t at = m_i;
  if (t.kind == Tok::kNeg && tokAt(static_cast<uint16_t>(at + 1)).kind == Tok::kNumber) {
    Node* n = make(NodeKind::kNumber, at);
    if (n == nullptr) return;
    n->milli = -tokAt(static_cast<uint16_t>(at + 1)).milli;
    f.node->add(n);
    m_i = static_cast<uint16_t>(m_i + 2);
    return;
  }
  Node* n = make(t.kind == Tok::kNumber ? NodeKind::kNumber : NodeKind::kWord, at);
  if (n == nullptr) return;
  n->milli = t.milli;
  f.node->add(n);
  ++m_i;
}

}  // namespace logo
}  // namespace cq
