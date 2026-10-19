#include "logo/parser.h"

#include <stdio.h>
#include <string.h>

#include "logo/words.h"

namespace cq {
namespace logo {

Node* Parser::make(NodeKind k, uint16_t tok) {
  Node* n = m_pool.make(k, tok);
  if (n == nullptr && !m_abort) tooMany("too many words", tok);
  return n;
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
  unclosed(p.to_tok, "TO", "END");
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
  if (m_root == nullptr) return nullptr;
  pushSeq(Close::kEof, m_root, 0);
  while (m_nf > 0 && !m_abort) {
    Frame& f = m_f[m_nf - 1];
    switch (f.kind) {  // VARIANT POINT: parser frames
      case FK::kSeq: stepSeq(f); break;
      case FK::kCall: stepCall(f); break;
      case FK::kExpr: stepExpr(f); break;
      case FK::kList: stepList(f); break;
    }
  }
  return m_root;
}

bool Parser::push(const Frame& f) {
  if (m_nf >= kMaxFrames) {
    tooMany("too deeply nested", m_i);
    return false;
  }
  m_f[m_nf++] = f;
  return true;
}

void Parser::pushSeq(Close c, Node* n, uint16_t open) {
  Frame f;
  f.kind = FK::kSeq;
  f.close = c;
  f.node = n;
  f.open_tok = open;
  push(f);
}

void Parser::pushExpr(bool full, Close c, bool wrap, uint16_t open) {
  Frame f;
  f.kind = FK::kExpr;
  f.close = c;
  f.full = full;
  f.wrap_template = wrap;
  f.open_tok = open;
  f.opnd_base = m_nopnd;
  f.op_base = m_nops;
  push(f);
}

void Parser::pushList(Node* n, uint16_t open, bool attached) {
  Frame f;
  f.kind = FK::kList;
  f.node = n;
  f.open_tok = open;
  f.attached = attached;
  push(f);
}

void Parser::startCall(int16_t word, uint16_t at) {
  Node* n = make(NodeKind::kCall, at);
  if (n == nullptr) return;
  n->ref = word;
  Frame f;
  f.kind = FK::kCall;
  f.node = n;
  f.open_tok = at;
  push(f);
}

void Parser::startProcCall(int16_t proc, uint16_t at) {
  Node* n = make(NodeKind::kProcCall, at);
  if (n == nullptr) return;
  n->ref = proc;
  Frame f;
  f.kind = FK::kCall;
  f.node = n;
  f.open_tok = at;
  push(f);
}

bool Parser::pushOperand(Node* n) {
  if (m_nopnd >= kMaxStack) {
    tooMany("too deeply nested", m_i);
    return false;
  }
  m_opnd[m_nopnd++] = n;
  return true;
}

void Parser::deliver(Node* n) {
  if (m_nf == 0 || n == nullptr) return;
  Frame& p = m_f[m_nf - 1];
  switch (p.kind) {
    case FK::kSeq: case FK::kList: p.node->add(n); break;
    case FK::kCall: p.node->add(n); ++p.next_input; break;
    case FK::kExpr:
      pushOperand(n);
      p.expect_operand = false;
      m_last_value_tok = static_cast<uint16_t>(m_i - 1);
      break;
  }
}

bool Parser::givesValue(const Token& t) const {
  if (t.kind != Tok::kName) return false;
  if (tokenIs(m_src, t, "TRUE") || tokenIs(m_src, t, "FALSE")) return true;
  const int16_t w = findWord(m_src, t);
  if (w >= 0) return gen::kWords[w].kind == gen::WordKind::kReporter;
  const int16_t p = findProc(t);
  return p >= 0 && m_procs[p].gives_value;
}

bool Parser::isCommandWord(const Token& t) const {
  if (t.kind != Tok::kName) return false;
  const int16_t w = findWord(m_src, t);
  if (w >= 0) {
    const gen::WordKind k = gen::kWords[w].kind;
    return k == gen::WordKind::kCommand || k == gen::WordKind::kControl;
  }
  const int16_t p = findProc(t);
  return p >= 0 && !m_procs[p].gives_value;
}

bool Parser::startsValue(const Token& t) const {
  switch (t.kind) {
    case Tok::kNumber: case Tok::kQuoted: case Tok::kInput: case Tok::kPlaceholder:
    case Tok::kLParen: case Tok::kLBracket: case Tok::kNeg: return true;
    case Tok::kName: return givesValue(t) || tokenIs(m_src, t, "NOT");
    default: return false;
  }
}

bool Parser::isStatementStart(const Token& t) const {
  return isCommandWord(t) || tokenIs(m_src, t, "END");
}

}  // namespace logo
}  // namespace cq
