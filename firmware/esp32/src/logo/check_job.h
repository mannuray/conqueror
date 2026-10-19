// One check of a program (Tank Logo LLD §20.2): lex → parse → check → compile → plan the path, in one workspace
// the caller owns (N3: the logo worker task's, in PSRAM, allocated once). Pure; host-tested.
#pragma once

#include <new>
#include <stdint.h>

#include "logo/checker.h"
#include "logo/compiler.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/program.h"
#include "logo/simulator.h"
#include "logo/vm.h"

namespace cq {
namespace logo {

constexpr uint16_t kSourceInbox = 8193;  // words.yaml source_bytes + 1: a longer text is refused (7010) unread

struct CheckSpace {
  static constexpr uint16_t kNodes = 4096;
  static constexpr uint16_t kPathPoints = 2000;

  CheckSpace() : pool(nodes, kNodes), parser(pool, diags) {}
  TokenList toks;
  Node nodes[kNodes];
  NodePool pool;
  Diagnostics diags;
  Parser parser;
  alignas(Compiler) unsigned char compiler_mem[sizeof(Compiler)];  // built per check (it keeps the source)
  Program program;
  Vm vm;
  PathPoint points[kPathPoints];
  PathBuffer path{points, kPathPoints, 0};
  CheckResult result;
  SimResult sim;
  bool compiled = false;
};

// Reads, checks, compiles and (when it compiled) plans the path. `src` must stay valid while the reply is written
// (words and messages point into it). True when the program can run (no error, compiled).
bool checkProgram(const char* src, uint16_t len, const Caps& caps, CheckSpace& s);

}  // namespace logo
}  // namespace cq
