#include "logo/parser.h"

#include <stdio.h>
#include <string.h>

#include "logo/words.h"

namespace cq {
namespace logo {

Node* Parser::make(NodeKind k, uint16_t tok) {
  Node* n = m_pool.make(k, tok);
  if (n == nullptr && !m_abort) {
    const DiagArg a[] = {{"what", "too many words"}};
    m_d.add(7010, tokAt(tok).line, tokAt(tok).col, a, 1);
    m_abort = true;
  }
  return n;
}

void Parser::error(uint16_t code, uint16_t at, const DiagArg* a, uint8_t n) {
  m_d.add(code, tokAt(at).line, tokAt(at).col, a, n);
}

int16_t Parser::findProc(const Token& t) const {
  if (t.kind != Tok::kName) return -1;
  for (uint8_t i = 0; i < m_n_procs; ++i) {
    const Token& n = tokAt(m_procs[i].name_tok);
    if (m_procs[i].valid && n.len == t.len && editDistance(m_src + n.start, n.len, m_src + t.start, t.len) == 0) {
      return i;
    }
  }
  return -1;
}

// 7008: a built-in word, preset or reserved word; 7007: defined twice. False: the definition is not usable.
bool Parser::nameAllowed(uint16_t at) {
  const Token& name = tokAt(at);
  char nm[40];
  tokenText(m_src, name, nm, sizeof nm);
  if (findWord(m_src, name) >= 0 || isAnyPreset(m_src, name) || isReserved(m_src, name) || tokenIs(m_src, name, "TO")) {
    const DiagArg a[] = {{"word", nm}};
    error(7008, at, a, 1);
    return false;
  }
  bool ok = true;
  for (uint8_t k = 0; k < m_n_procs; ++k) {
    const Token& o = tokAt(m_procs[k].name_tok);
    if (o.len != name.len || editDistance(m_src + o.start, o.len, m_src + name.start, name.len) != 0) continue;
    char la[8], lb[8];
    snprintf(la, sizeof la, "%u", static_cast<unsigned>(o.line));
    snprintf(lb, sizeof lb, "%u", static_cast<unsigned>(name.line));
    const DiagArg a[] = {{"word", nm}, {"a", la}, {"b", lb}};
    error(7007, at, a, 3);
    ok = false;
  }
  return ok;
}

// From the first body token to its END (outside brackets): OUTPUT makes it give a value; a nested TO is 7017;
// no END is 7003. Returns the token after END (or the end of the text).
uint16_t Parser::scanBody(ProcInfo& p, uint16_t i) {
  int d = 0;
  for (; tokAt(i).kind != Tok::kEnd; ++i) {
    const Token& b = tokAt(i);
    if (b.kind == Tok::kLBracket) {
      ++d;
    } else if (b.kind == Tok::kRBracket) {
      --d;
    } else if (tokenIs(m_src, b, "TO")) {
      error(7017, i);
    } else if (tokenIs(m_src, b, "OUTPUT") || tokenIs(m_src, b, "OP")) {
      p.gives_value = true;
    } else if (tokenIs(m_src, b, "END") && d <= 0) {
      return static_cast<uint16_t>(i + 1);
    }
  }
  char line[8];
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(tokAt(p.to_tok).line));
  const DiagArg a[] = {{"bracket", "TO"}, {"line", line}, {"closer", "END"}};
  error(7003, p.to_tok, a, 3);
  return i;
}

void Parser::addProc(const ProcInfo& p) {
  if (m_n_procs < kMaxProcs) {
    m_procs[m_n_procs++] = p;
    return;
  }
  const DiagArg a[] = {{"what", "more than 64 words of your own"}};
  error(7010, p.to_tok, a, 1);
}

// At TO: the name, its inputs, the body to END. Returns the token after the definition.
uint16_t Parser::readDefinition(uint16_t i) {
  ProcInfo p;
  p.to_tok = i++;
  if (tokAt(i).kind != Tok::kName) {
    const DiagArg a[] = {{"word", "TO"}, {"what", "a name"}, {"example", "TO SQUARE :size"}};
    error(7002, p.to_tok, a, 3);
    return i;
  }
  p.name_tok = i;
  p.valid = nameAllowed(i);
  p.first_input_tok = ++i;
  while (tokAt(i).kind == Tok::kInput) {
    ++p.n_inputs;
    ++i;
  }
  i = scanBody(p, i);
  addProc(p);
  return i;
}

void Parser::collectDefinitions() {
  m_n_procs = 0;
  uint16_t i = 0;
  int depth = 0;
  while (tokAt(i).kind != Tok::kEnd) {
    const Token& t = tokAt(i);
    if (t.kind == Tok::kLBracket) ++depth;
    if (t.kind == Tok::kRBracket && depth > 0) --depth;
    if (!tokenIs(m_src, t, "TO")) {
      ++i;
    } else if (depth > 0) {
      error(7017, i++);  // TO inside a block
    } else {
      i = readDefinition(i);
    }
  }
}

Node* Parser::parse(const char* src, const TokenList& toks) {
  m_src = src;
  m_toks = &toks;
  m_i = 0;
  m_abort = false;
  m_nf = m_nopnd = m_nops = 0;
  m_last_value_tok = 0xFFFF;
  collectDefinitions();
  m_root = make(NodeKind::kProgram, 0);
  return m_root;  // Task 5 adds pass 2 here
}

}  // namespace logo
}  // namespace cq
