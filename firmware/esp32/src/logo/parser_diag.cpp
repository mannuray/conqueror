// Parser diagnostics and recovery (grammar.md §11, LLD §5): the friendly messages, the minus hint, and skipping to
// the next statement after an error.
#include <stdio.h>
#include <string.h>

#include "logo/parser.h"
#include "logo/words.h"

namespace cq {
namespace logo {

void Parser::error(uint16_t code, uint16_t at, const DiagArg* a, uint8_t n) {
  m_d.add(code, tokAt(at).line, tokAt(at).col, a, n);
}

// 7010: a fixed capacity ran out; reading stops.
void Parser::tooMany(const char* what, uint16_t at) {
  const DiagArg a[] = {{"what", what}};
  error(7010, at, a, 1);
  m_abort = true;
}

// 7003: the bracket (or TO) opened at `open` is never closed.
void Parser::unclosed(uint16_t open, const char* bracket, const char* closer) {
  char line[8];
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(tokAt(open).line));
  const DiagArg a[] = {{"bracket", bracket}, {"line", line}, {"closer", closer}};
  error(7003, open, a, 3);
}

void Parser::minusHint(uint16_t at) {
  if (tokAt(at).kind != Tok::kNeg || m_last_value_tok == 0xFFFF || m_last_value_tok + 1 != at) return;
  char a[40], b[40], hint[112];
  tokenText(m_src, tokAt(m_last_value_tok), a, sizeof a);
  const Token& n = tokAt(static_cast<uint16_t>(at + 1));
  tokenText(m_src, n, b, sizeof b);
  snprintf(hint, sizeof hint, " To subtract, write %s - %s.", a, b);
  m_d.appendToLast(hint);
}

void Parser::unknownWord(uint16_t at) {
  const Token& t = tokAt(at);
  char nm[40], sug[40] = "";
  tokenText(m_src, t, nm, sizeof nm);
  uint8_t best = 3;
  for (uint8_t i = 0; i < m_n_procs; ++i) {
    const Token& p = tokAt(m_procs[i].name_tok);
    const uint8_t d = editDistance(m_src + t.start, t.len, m_src + p.start, p.len);
    if (m_procs[i].valid && d < best) { best = d; tokenText(m_src, p, sug, sizeof sug); }
  }
  for (uint8_t i = 0; i < gen::kWordCount; ++i) {
    const char* w = gen::kWords[i].name;
    const uint8_t d = editDistance(m_src + t.start, t.len, w, static_cast<uint16_t>(strlen(w)));
    if (d < best) { best = d; snprintf(sug, sizeof sug, "%s", w); }
  }
  if (best > 2) sug[0] = '\0';
  const DiagArg a[] = {{"word", nm}, {"suggestion", sug}};
  error(7001, at, a, 2);
}

void Parser::leftover(uint16_t at) {
  char v[40];
  if (tokAt(at).kind == Tok::kLBracket) snprintf(v, sizeof v, "a list [ … ]");
  else tokenText(m_src, tokAt(at), v, sizeof v);
  const DiagArg a[] = {{"value", v}};
  error(7004, at, a, 1);
  minusHint(at);
}

void Parser::inputSpec(const Frame& call, gen::InType& type, const char*& what, const char*& example,
                       const char*& name) const {
  static char pex[48];
  if (call.node->kind == NodeKind::kCall) {
    const gen::WordSpec& w = gen::kWords[call.node->ref];
    const gen::InputSpec& in = w.inputs[call.next_input];
    type = in.type;
    what = w.missing[0] != '\0' ? w.missing : in.what;
    example = w.example;
    name = w.name;
  } else {
    const ProcInfo& p = m_procs[call.node->ref];
    type = gen::InType::kAny;
    what = "a value";
    char nm[40];
    tokenText(m_src, tokAt(p.name_tok), nm, sizeof nm);
    snprintf(pex, sizeof pex, "%s 10", nm);
    example = pex;
    name = "";
  }
}

void Parser::missingInput(const Frame& call) {
  gen::InType type;
  const char *what, *example, *name;
  inputSpec(call, type, what, example, name);
  char nm[40];
  if (name[0] == '\0') tokenText(m_src, tokAt(call.open_tok), nm, sizeof nm);
  else snprintf(nm, sizeof nm, "%s", name);
  const DiagArg a[] = {{"word", nm}, {"what", what}, {"example", example}};
  error(7002, call.open_tok, a, 3);
}

void Parser::unwindToSeq() {
  while (m_nf > 0 && m_f[m_nf - 1].kind != FK::kSeq) {
    const Frame& f = m_f[m_nf - 1];
    if (f.kind == FK::kExpr) { m_nopnd = f.opnd_base; m_nops = f.op_base; }
    --m_nf;
  }
}

// Steps over the token that failed (never past the end or a closing ]); returns the bracket depth it opened.
int Parser::resyncStart() {
  const Tok k = tok().kind;
  if (k == Tok::kEnd || k == Tok::kRBracket) return 0;
  ++m_i;
  return k == Tok::kLBracket ? 1 : 0;
}

// Skips to the next statement: a new line, a command word or END, or the ] that closes this block.
void Parser::resync() {
  const uint16_t line0 = tok().line;
  int depth = resyncStart();
  while (tok().kind != Tok::kEnd) {
    const Token& t = tok();
    if (depth == 0 && (t.kind == Tok::kRBracket || t.line > line0 || isStatementStart(t))) return;
    if (t.kind == Tok::kLBracket) ++depth;
    if (t.kind == Tok::kRBracket && depth > 0) --depth;
    ++m_i;
  }
}

void Parser::fail(bool skip) {
  unwindToSeq();
  if (skip) resync();
}

}  // namespace logo
}  // namespace cq
