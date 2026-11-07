// Checker: LAUNCH (Tank Logo LLD §26.3) - only the main program moves the tank. A word that moves the tank, its
// camera or where it may go (gen::WordSpec::moves), or a word of your own that reaches one, inside a LAUNCH block is
// 7057 at that call; an OUTPUT inside it is 7059. Iterative walks over a bounded stack; recursion between your words ends (a fixed point).
#include <stdio.h>
#include <string.h>

#include "logo/checker.h"
#include "logo/generated/words.h"
#include "logo/lexer.h"

namespace cq {
namespace logo {
namespace {

constexpr uint16_t kMaxDepth = 256;

// Depth first over `first`, its siblings and everything below them (depth-bounded, never width-bounded).
class Walk {
 public:
  explicit Walk(const Node* first) {
    if (first != nullptr) m_st[m_n++] = first;
  }
  const Node* next() {
    if (m_n == 0) return nullptr;
    const Node* n = m_st[m_n - 1];
    m_st[m_n - 1] = n->next;
    if (m_st[m_n - 1] == nullptr) --m_n;
    if (n->first != nullptr && m_n < kMaxDepth) m_st[m_n++] = n->first;
    return n;
  }

 private:
  const Node* m_st[kMaxDepth];
  uint16_t m_n = 0;
};

bool movesBuiltIn(const Node* n) {
  return n->kind == NodeKind::kCall && n->ref >= 0 && gen::kWords[n->ref].moves;
}

bool movesOwn(const Node* n, const bool* moves, uint8_t n_procs) {
  return n->kind == NodeKind::kProcCall && n->ref >= 0 && n->ref < n_procs && moves[n->ref];
}

// The first call below `first` (siblings included) that moves the tank, or nullptr.
const Node* firstMove(const Node* first, const bool* moves, uint8_t n_procs) {
  Walk w(first);
  for (const Node* n = w.next(); n != nullptr; n = w.next()) {
    if (movesBuiltIn(n) || movesOwn(n, moves, n_procs)) return n;
  }
  return nullptr;
}

bool isCallOf(const Node* n, const char* name) {
  return n->kind == NodeKind::kCall && n->ref >= 0 && strcmp(gen::kWords[n->ref].name, name) == 0;
}

// An OUTPUT below `first`: the launched part has no word to give a value back from (7059, final review I2).
const Node* firstOutput(const Node* first) {
  Walk w(first);
  for (const Node* n = w.next(); n != nullptr; n = w.next()) {
    if (isCallOf(n, "OUTPUT")) return n;
  }
  return nullptr;
}

// Which of your words reach a move: one more word found per pass, so at most one pass per word.
void markMovingWords(const Node* program, bool* moves, uint8_t n_procs) {
  for (bool changed = true; changed;) {
    changed = false;
    for (const Node* d = program->first; d != nullptr; d = d->next) {
      const bool def = d->kind == NodeKind::kDef && d->ref >= 0 && d->ref < n_procs;
      if (def && !moves[d->ref] && firstMove(d->last, moves, n_procs) != nullptr) {
        moves[d->ref] = changed = true;
      }
    }
  }
}

}  // namespace

void Checker::checkLaunch(const Node* program) {
  bool moves[gen::kMaxProcs] = {};
  const uint8_t n_procs = m_p.procCount();
  markMovingWords(program, moves, n_procs);
  Walk w(program->first);
  for (const Node* launch = w.next(); launch != nullptr; launch = w.next()) {
    if (!isCallOf(launch, "LAUNCH")) continue;
    if (const Node* o = firstOutput(launch->first)) error(7059, o->tok);
    const Node* n = firstMove(launch->first, moves, n_procs);
    if (n == nullptr) continue;
    char word[25];
    if (movesBuiltIn(n)) snprintf(word, sizeof word, "%s", gen::kWords[n->ref].name);
    else tokenText(m_src, m_toks.t[n->tok], word, sizeof word);
    const DiagArg a[] = {{"word", word}};
    error(7057, n->tok, a, 1);  // one per LAUNCH
  }
}

}  // namespace logo
}  // namespace cq
