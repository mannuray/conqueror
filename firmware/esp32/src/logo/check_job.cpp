#include "logo/check_job.h"

#include "logo/generated/words.h"

namespace cq {
namespace logo {

bool checkProgram(const char* src, uint16_t len, const Caps& caps, CheckSpace& s) {
  s.diags.clear();
  s.pool.reset();
  s.program.clear();
  s.path.n = 0;
  s.result = CheckResult();
  s.sim = SimResult();
  s.compiled = false;
  if (len > gen::kSourceBytes) {
    const DiagArg a[] = {{"what", "more than 8192 letters"}};
    s.diags.add(7010, 1, 1, a, 1);
    return false;
  }
  lex(src, len, s.toks, s.diags);
  Node* prog = s.parser.parse(src, s.toks);
  s.result = Checker(s.parser, src, s.toks, s.diags).check(prog, caps);
  if (!s.result.ok) return false;
  Compiler* c = new (s.compiler_mem) Compiler(s.parser, src, s.toks, s.diags);
  s.compiled = c->compile(prog, s.result.handles_obstacles, s.program);
  c->~Compiler();
  if (!s.compiled) return false;
  s.sim = Simulator(s.vm).plan(s.program, s.path);
  return true;
}

}  // namespace logo
}  // namespace cq
