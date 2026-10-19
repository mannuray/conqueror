// Parser pass 2, expressions (grammar.md §6-§8): operator precedence on two stacks, inputs that stop before
// comparisons, one comparison per condition, ( ) and [ ] around a template.
#include <stdio.h>

#include "logo/parser.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace {

// Indexed by Op: lower binds tighter (grammar.md §6). 4 = the comparisons.
constexpr uint8_t kPrec[] = {9, 3, 3, 2, 2, 4, 4, 4, 4, 4, 4, 6, 7, 5, 1};
static_assert(sizeof kPrec == static_cast<size_t>(Op::kNeg) + 1, "one precedence per Op");
constexpr uint8_t kComparePrec = 4;

uint8_t prec(Op o) { return kPrec[static_cast<uint8_t>(o)]; }
bool isCompare(Op o) { return prec(o) == kComparePrec; }
bool isUnary(Op o) { return o == Op::kNeg || o == Op::kNot; }

struct TokOp {
  Tok tok;
  Op op;
};
constexpr TokOp kBinary[] = {{Tok::kPlus, Op::kAdd}, {Tok::kMinus, Op::kSub}, {Tok::kStar, Op::kMul},
                             {Tok::kSlash, Op::kDiv}, {Tok::kEq, Op::kEq},     {Tok::kNe, Op::kNe},
                             {Tok::kLt, Op::kLt},     {Tok::kGt, Op::kGt},     {Tok::kLe, Op::kLe},
                             {Tok::kGe, Op::kGe}};

Op binaryOp(const char* src, const Token& t) {
  for (const TokOp& e : kBinary) {
    if (e.tok == t.kind) return e.op;
  }
  if (t.kind != Tok::kName) return Op::kNone;
  if (tokenIs(src, t, "AND")) return Op::kAnd;
  return tokenIs(src, t, "OR") ? Op::kOr : Op::kNone;
}

struct TokLeaf {
  Tok tok;
  NodeKind kind;
};
constexpr TokLeaf kLeaves[] = {{Tok::kNumber, NodeKind::kNumber},
                               {Tok::kQuoted, NodeKind::kQuoted},
                               {Tok::kInput, NodeKind::kInput},
                               {Tok::kPlaceholder, NodeKind::kPlaceholder}};

// The leaf a token makes on its own; kProgram when it is not a leaf.
NodeKind leafKind(Tok k) {
  for (const TokLeaf& e : kLeaves) {
    if (e.tok == k) return e.kind;
  }
  return NodeKind::kProgram;
}

}  // namespace

void Parser::stepExpr(Frame& f) {
  if (f.expect_operand) { exprOperand(f); return; }
  const Op op = binaryOp(m_src, tok());
  if (op != Op::kNone) { exprOperator(f, op); return; }
  if (f.close == Close::kParen || f.close == Close::kBracket) { exprClose(f); return; }
  finishExpr();
}

void Parser::exprOperand(Frame& f) {
  const Token& t = tok();
  const uint16_t at = m_i;
  const NodeKind leaf = leafKind(t.kind);
  if (leaf != NodeKind::kProgram) {
    Node* n = make(leaf, at);
    if (n == nullptr) return;
    n->milli = t.milli;
    n->ph = t.ph;
    ++m_i;
    deliver(n);
    return;
  }
  switch (t.kind) {
    case Tok::kLParen: ++m_i; pushExpr(true, Close::kParen, false, at); return;
    case Tok::kLBracket: openList(at); return;
    case Tok::kNeg:
      if (pushOp(Op::kNeg, at)) ++m_i;
      return;
    case Tok::kName: nameOperand(f); return;
    default: missingOperand(f); return;
  }
}

// TRUE / FALSE as a value. True when the word was one of them.
bool Parser::boolOperand(const Token& t) {
  const bool yes = tokenIs(m_src, t, "TRUE");
  if (!yes && !tokenIs(m_src, t, "FALSE")) return false;
  Node* b = make(NodeKind::kBool, m_i);
  if (b == nullptr) return true;
  b->milli = yes ? 1 : 0;
  ++m_i;
  deliver(b);
  return true;
}

// TRUE / FALSE, NOT, a reporter or a word of our own that gives a value.
void Parser::nameOperand(const Frame& f) {
  const Token& t = tok();
  const uint16_t at = m_i;
  if (boolOperand(t)) return;
  if (tokenIs(m_src, t, "NOT")) { notOperand(f); return; }
  const int16_t w = findWord(m_src, t);
  const int16_t p = w >= 0 ? -1 : findProc(t);
  if (givesValue(t)) {
    ++m_i;
    if (w >= 0) startCall(w, at);
    else startProcCall(p, at);
    return;
  }
  if (w >= 0 || p >= 0) notAValue(at, w);
  else unknownWord(at);
  fail(true);
}

// NOT only in a condition (7005 elsewhere).
void Parser::notOperand(const Frame& f) {
  if (!f.full) {
    const DiagArg a[] = {{"word", "NOT"}};
    error(7005, m_i, a, 1);
    fail(true);
    return;
  }
  if (pushOp(Op::kNot, m_i)) ++m_i;
}

// 7005: a command (built-in or our own) where a value is needed.
void Parser::notAValue(uint16_t at, int16_t word) {
  char nm[40];
  tokenText(m_src, tokAt(at), nm, sizeof nm);
  const DiagArg a[] = {{"word", word >= 0 ? gen::kWords[word].name : nm}};
  error(7005, at, a, 1);
}

// 7002: an operator (or an empty ( )) with nothing after it.
void Parser::missingOperand(const Frame& f) {
  const bool after_op = m_nops > f.op_base;
  const uint16_t op_tok = after_op ? m_ops[m_nops - 1].tok : f.open_tok;
  char nm[40];
  if (after_op) tokenText(m_src, tokAt(op_tok), nm, sizeof nm);
  else snprintf(nm, sizeof nm, "( )");
  const DiagArg a[] = {{"word", nm}, {"what", "a value"}, {"example", "10 + 5"}};
  error(7002, op_tok, a, 3);
  fail(false);
}

void Parser::exprOperator(Frame& f, Op op) {
  const uint16_t at = m_i;
  if (!f.full && prec(op) >= kComparePrec) { finishExpr(); return; }  // grammar.md §6: inputs stop before comparisons
  if (isCompare(op)) {
    if (f.seen_compare) { error(7018, at); fail(true); return; }
    f.seen_compare = true;
  }
  if (op == Op::kAnd || op == Op::kOr) f.seen_compare = false;
  reduceFor(f, op);
  if (!pushOp(op, at)) return;
  ++m_i;
  f.expect_operand = true;
}

// Reduces every waiting operator that binds at least as tight as `op` (left to right).
void Parser::reduceFor(const Frame& f, Op op) {
  while (m_nops > f.op_base && prec(m_ops[m_nops - 1].op) <= prec(op) && !m_abort) reduceTop();
}

bool Parser::pushOp(Op op, uint16_t at) {
  if (m_nops >= kMaxStack) {
    tooMany("too deeply nested", at);
    return false;
  }
  m_ops[m_nops++] = {op, at};
  return true;
}

// Inside ( ) or a [ ] template: the closer ends it; another value is 7028 / 7004; anything else is unclosed.
void Parser::exprClose(const Frame& f) {
  const bool paren = f.close == Close::kParen;
  if (tok().kind == (paren ? Tok::kRParen : Tok::kRBracket)) { ++m_i; finishExpr(); return; }
  if (startsValue(tok())) { extraValue(f); return; }
  unclosed(f.open_tok, paren ? "(" : "[", paren ? ")" : "]");
  fail(false);
}

void Parser::extraValue(const Frame& f) {
  const uint16_t at = m_i;
  char a1[40], b1[40];
  tokenText(m_src, tokAt(m_last_value_tok == 0xFFFF ? f.open_tok : m_last_value_tok), a1, sizeof a1);
  tokenText(m_src, tok(), b1, sizeof b1);
  if (f.close == Close::kParen) {
    const DiagArg a[] = {{"a", a1}, {"b", b1}};
    error(7028, at, a, 2);
  } else {
    const DiagArg a[] = {{"value", b1}};
    error(7004, at, a, 1);
  }
  minusHint(at);
  fail(true);
}

void Parser::reduceTop() {
  const OpEntry e = m_ops[--m_nops];
  if (isUnary(e.op)) {
    Node* a = m_opnd[--m_nopnd];
    if (e.op == Op::kNeg && a->kind == NodeKind::kNumber) {
      a->milli = -a->milli;
      a->tok = e.tok;
      m_opnd[m_nopnd++] = a;
      return;
    }
    Node* u = make(NodeKind::kUnary, e.tok);
    if (u == nullptr) return;
    u->op = e.op;
    u->add(a);
    m_opnd[m_nopnd++] = u;
    return;
  }
  Node* b = m_opnd[--m_nopnd];
  Node* a = m_opnd[--m_nopnd];
  Node* n = make(NodeKind::kBinary, e.tok);
  if (n == nullptr) return;
  n->op = e.op;
  n->add(a);
  n->add(b);
  m_opnd[m_nopnd++] = n;
}

void Parser::finishExpr() {
  const Frame f = m_f[m_nf - 1];
  while (m_nops > f.op_base && !m_abort) reduceTop();
  if (m_abort) return;
  Node* n = m_nopnd > f.opnd_base ? m_opnd[m_nopnd - 1] : nullptr;
  m_nopnd = f.opnd_base;
  m_nops = f.op_base;
  --m_nf;
  if (n != nullptr && f.wrap_template) {
    Node* t = make(NodeKind::kTemplate, f.open_tok);
    if (t == nullptr) return;
    t->add(n);
    n = t;
  }
  deliver(n);
}

}  // namespace logo
}  // namespace cq
