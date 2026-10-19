// Tank Logo compiler (LLD §7): a checked tree → instructions. Iterative (an explicit work stack: one entry per
// tree level, a step counter and jump-patch slots per entry) - no recursion. Runs only on a tree the Checker passed.
#pragma once

#include <stdint.h>

#include "logo/ast.h"
#include "logo/diag.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/program.h"

namespace cq {
namespace logo {

// What a built-in word compiles to.
enum class WordRole : uint8_t {
  kNone, kEffect, kRepeat, kForever, kIf, kIfElse, kWhile, kWaitUntil, kStop, kOutput, kSensor, kBuiltin
};

class Compiler {
 public:
  static constexpr uint16_t kMaxWork = 256;

  Compiler(const Parser& p, const char* src, const TokenList& toks, Diagnostics& d)
      : m_parser(p), m_src(src), m_toks(toks), m_d(d) {}
  // False when the program does not fit (7010) - the reason is in Diagnostics.
  bool compile(const Node* program, bool handles_obstacles, Program& out);

 private:
  struct Work {
    const Node* n;
    const Node* next;  // the next child to compile
    int16_t proc;      // the word being compiled, -1 for the program
    uint8_t step;
    bool stmt;         // a statement (a child of a block or the program), not a value
    uint16_t p1;       // jump to patch / loop start
    uint16_t p2;
  };

  // compiler.cpp
  bool stepNode(Work& w);  // true when the node is done
  bool stepProgram(Work& w);
  bool stepDef(Work& w);
  bool stepBlock(Work& w);
  bool child(Work& w);  // pushes the next child; false when there is none
  void pushWork(const Node* n, int16_t proc);
  uint16_t emit(OpCode op, uint8_t a = 0, int32_t n = 0);
  void patch(uint16_t at);
  void line(const Node* n);
  void full(const Node* n, const char* what);
  WordRole role(int16_t word, uint8_t& arg) const;
  // compiler_expr.cpp
  bool leaf(const Work& w);
  bool stepOperator(Work& w);
  bool stepProcCall(Work& w);
  bool stepReporter(Work& w, WordRole r, uint8_t arg);
  int16_t wordConst(const Token& t, uint16_t skip);
  int16_t listConst(const Node* list);
  bool listItem(const Node* item, Value& out);
  int8_t inputIndex(int16_t proc, const Node* n) const;
  // compiler_stmt.cpp
  bool stepCall(Work& w);
  bool stepEffect(Work& w, uint8_t kind);

  const Parser& m_parser;
  const char* m_src;
  const TokenList& m_toks;
  Diagnostics& m_d;
  Program* m_out = nullptr;
  Work m_st[kMaxWork];
  uint16_t m_ns = 0;
  bool m_failed = false;
};

}  // namespace logo
}  // namespace cq
