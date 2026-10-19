// Tank Logo parser (grammar.md §3-§9, LLD §5): pass 1 finds every TO; pass 2 reads statements with an explicit
// frame stack (no recursion, coding standards §2.3). Errors go to Diagnostics; the tree is always returned.
#pragma once

#include <stdint.h>

#include "logo/ast.h"
#include "logo/diag.h"
#include "logo/generated/words.h"
#include "logo/lexer.h"

namespace cq {
namespace logo {

struct ProcInfo {
  uint16_t to_tok = 0;
  uint16_t name_tok = 0;
  uint16_t first_input_tok = 0;
  uint8_t n_inputs = 0;
  bool gives_value = false;
  bool valid = false;
};

class Parser {
 public:
  static constexpr uint8_t kMaxProcs = 64;
  static constexpr uint8_t kMaxFrames = 64;
  static constexpr uint8_t kMaxStack = 128;

  Parser(NodePool& pool, Diagnostics& diags) : m_pool(pool), m_d(diags) {}
  Node* parse(const char* src, const TokenList& toks);
  uint8_t procCount() const { return m_n_procs; }
  const ProcInfo& proc(uint8_t i) const { return m_procs[i]; }
  int16_t findProc(const Token& t) const;  // valid procedures only

 private:
  enum class FK : uint8_t { kSeq, kCall, kExpr, kList };
  enum class Close : uint8_t { kNone, kEof, kBracket, kEnd, kParen };
  struct Frame {
    FK kind = FK::kSeq;
    Close close = Close::kNone;
    Node* node = nullptr;
    uint16_t open_tok = 0;
    uint8_t next_input = 0;
    bool attached = true;     // kSeq / kList: already linked into the tree
    bool full = false;        // kExpr: a condition (comparisons, AND / OR)
    bool wrap_template = false;
    bool expect_operand = true;
    bool seen_compare = false;
    uint8_t opnd_base = 0;
    uint8_t op_base = 0;
  };
  struct OpEntry {
    Op op;
    uint16_t tok;
  };

  // pass 1
  void collectDefinitions();
  uint16_t readDefinition(uint16_t i);
  bool nameAllowed(uint16_t at);
  uint16_t scanBody(ProcInfo& p, uint16_t i);
  void addProc(const ProcInfo& p);
  // pass 2 steps
  void stepSeq(Frame& f);
  void stepCall(Frame& f);
  void stepExpr(Frame& f);
  void stepList(Frame& f);
  void parseDef();
  void skipToEnd();
  // frames
  bool push(const Frame& f);
  void pushSeq(Close c, Node* n, uint16_t open);
  void pushExpr(bool full, Close c, bool wrap, uint16_t open);
  void pushList(Node* n, uint16_t open, bool attached);
  void startCall(int16_t word, uint16_t tok);
  void startProcCall(int16_t proc, uint16_t tok);
  void deliver(Node* n);
  void finishExpr();
  void reduceTop();
  bool pushOperand(Node* n);
  // helpers
  const Token& tok() const { return m_toks->t[m_i]; }
  const Token& tokAt(uint16_t i) const { return m_toks->t[i]; }
  Node* make(NodeKind k, uint16_t tok);
  bool startsValue(const Token& t) const;
  bool givesValue(const Token& t) const;
  bool isStatementStart(const Token& t) const;
  bool isCommandWord(const Token& t) const;
  void error(uint16_t code, uint16_t tok, const DiagArg* a = nullptr, uint8_t n = 0);
  void minusHint(uint16_t at);
  void unknownWord(uint16_t at);
  void leftover(uint16_t at);
  void missingInput(const Frame& call);
  void fail(bool skip);
  void unwindToSeq();
  void resync();
  void inputSpec(const Frame& call, gen::InType& type, const char*& what, const char*& example,
                 const char*& name) const;

  NodePool& m_pool;
  Diagnostics& m_d;
  const char* m_src = "";
  const TokenList* m_toks = nullptr;
  uint16_t m_i = 0;
  bool m_abort = false;
  uint16_t m_last_value_tok = 0xFFFF;
  ProcInfo m_procs[kMaxProcs];
  uint8_t m_n_procs = 0;
  Frame m_f[kMaxFrames];
  uint8_t m_nf = 0;
  Node* m_opnd[kMaxStack];
  uint8_t m_nopnd = 0;
  OpEntry m_ops[kMaxStack];
  uint8_t m_nops = 0;
  Node* m_root = nullptr;
};

}  // namespace logo
}  // namespace cq
