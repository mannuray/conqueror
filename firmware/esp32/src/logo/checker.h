// Tank Logo checker (LLD §6, grammar.md §8 / §11): scope, value vs command, needs, level, constant ranges,
// templates, BLOCKED?, tail positions. Walks the tree with an explicit stack (no recursion).
#pragma once

#include <stdint.h>

#include "logo/ast.h"
#include "logo/diag.h"
#include "logo/generated/words.h"
#include "logo/lexer.h"
#include "logo/parser.h"

namespace cq {
namespace logo {

struct Caps {
  bool encoders = true;
  bool ai = false;      // an AI program (vision words allowed)
  uint8_t version = 1;  // v1: level-3 words other than OUTPUT are reserved (7030)
};

struct CheckResult {
  bool ok = false;
  bool handles_obstacles = false;  // uses BLOCKED? (design-notes §82 h)
  uint64_t tail_callees = 0;       // bit i: procedure i is called in tail position
};

class Checker {
 public:
  Checker(const Parser& p, const char* src, const TokenList& toks, Diagnostics& d)
      : m_p(p), m_src(src), m_toks(toks), m_d(d) {}
  CheckResult check(Node* program, const Caps& caps);

 private:
  struct Item {
    Node* n;
    int16_t def;  // the definition we are inside, -1 at top level, kParamList in a definition's own inputs
    bool in_template;
  };
  static constexpr int16_t kParamList = -2;
  static constexpr int16_t kSkipChildren = -3;
  static constexpr uint16_t kMaxWalk = 256;

  int16_t visit(const Item& it, const Caps& caps, CheckResult& r);
  void pushChildren(const Item& it, int16_t def, Item* st, uint16_t& ns) const;
  void visitCall(Node* n, int16_t def, const Caps& caps, CheckResult& r);
  void needsAndLevel(const gen::WordSpec& w, uint16_t tok, const Caps& caps);
  void checkRange(const Node* call, const gen::WordSpec& w, uint8_t i, const Node* c);
  void arcTooTight(const Node* call, const Node* radius);
  void visitInput(const Node* n, int16_t def);
  void listWarning(const Node* list);
  void markTails(Node* body, CheckResult& r);
  void error(uint16_t code, uint16_t tok, const DiagArg* a = nullptr, uint8_t n = 0);

  const Parser& m_p;
  const char* m_src;
  const TokenList& m_toks;
  Diagnostics& m_d;
};

}  // namespace logo
}  // namespace cq
