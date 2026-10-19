#include "logo/printer.h"

#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

class Out {
 public:
  Out(char* b, size_t c) : m_b(b), m_c(c) { m_b[0] = '\0'; }
  void put(const char* s) {
    const size_t n = strlen(m_b);
    const char last = n > 0 ? m_b[n - 1] : '(';
    const bool space = last != '(' && last != '[' && last != '{' && s[0] != ')' && s[0] != ']' && s[0] != '}';
    if (space) strncat(m_b, " ", m_c - strlen(m_b) - 1);
    strncat(m_b, s, m_c - strlen(m_b) - 1);
  }

 private:
  char* m_b;
  size_t m_c;
};

void upperCopy(const char* src, const Token& t, char* out, size_t cap) {
  size_t i = 0;
  for (; i < t.len && i + 1 < cap; ++i) {
    const char c = src[t.start + i];
    out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
  }
  out[i] = '\0';
}

void number(int32_t milli, char* out, size_t cap) {
  const bool neg = milli < 0;
  const int64_t v = neg ? -static_cast<int64_t>(milli) : milli;
  if (v % 1000 == 0) {
    snprintf(out, cap, "%s%lld", neg ? "-" : "", static_cast<long long>(v / 1000));
    return;
  }
  char frac[8];
  snprintf(frac, sizeof frac, "%03lld", static_cast<long long>(v % 1000));
  for (int i = 2; i > 0 && frac[i] == '0'; --i) frac[i] = '\0';
  snprintf(out, cap, "%s%lld.%s", neg ? "-" : "", static_cast<long long>(v / 1000), frac);
}

// Indexed by Op.
constexpr const char* kOpNames[] = {"?", "+", "-", "*", "/", "=", "<>", "<", ">", "<=", ">=", "AND", "OR", "NOT", "-"};
static_assert(sizeof kOpNames / sizeof kOpNames[0] == static_cast<size_t>(Op::kNeg) + 1, "one name per Op");

// Indexed by NodeKind: the text that closes a node ("" for none).
constexpr const char* kClosers[] = {"", ")", ")", "]", "]", "}", ")", ")", "", "", "", "", "", "", "", ")", ")"};
static_assert(sizeof kClosers / sizeof kClosers[0] == static_cast<size_t>(NodeKind::kBinary) + 1, "one per kind");

// Indexed by NodeKind: the bracket that opens a block, template, list or ( ) of inputs ("" for the others).
constexpr const char* kOpeners[] = {"", "", "(", "[", "[", "{", "", "", "", "", "", "", "", "", "", "", ""};
static_assert(sizeof kOpeners / sizeof kOpeners[0] == static_cast<size_t>(NodeKind::kBinary) + 1, "one per kind");

constexpr const char* kPlaceholders[] = {"?", "?1", "?2"};

struct Ctx {
  const char* src;
  const TokenList& toks;
  const Parser& p;
};

void procName(const Ctx& c, const Node* n, const char* prefix, char* buf, size_t cap) {
  const size_t k = strlen(prefix);
  memcpy(buf, prefix, k);
  upperCopy(c.src, c.toks.t[c.p.proc(static_cast<uint8_t>(n->ref)).name_tok], buf + k, cap - k);
}

// A leaf's text, or the opening bracket of a block, template, list or ( ) of inputs.
void leafText(const Ctx& c, const Node* n, char* buf, size_t cap) {
  const Token& t = c.toks.t[n->tok];
  switch (n->kind) {
    case NodeKind::kNumber: number(n->milli, buf, cap); return;
    case NodeKind::kWord: case NodeKind::kQuoted: case NodeKind::kInput: tokenText(c.src, t, buf, cap); return;
    case NodeKind::kPreset: upperCopy(c.src, t, buf, cap); return;
    case NodeKind::kPlaceholder: snprintf(buf, cap, "%s", kPlaceholders[n->ph % 3]); return;
    case NodeKind::kBool: snprintf(buf, cap, "%s", n->milli != 0 ? "TRUE" : "FALSE"); return;
    default: snprintf(buf, cap, "%s", kOpeners[static_cast<uint8_t>(n->kind)]); return;
  }
}

// The text that opens node n ("" for the program).
void openText(const Ctx& c, const Node* n, char* buf, size_t cap) {
  switch (n->kind) {  // VARIANT POINT: node kinds
    case NodeKind::kDef: procName(c, n, "(TO ", buf, cap); return;
    case NodeKind::kProcCall: procName(c, n, "(", buf, cap); return;
    case NodeKind::kCall: {
      const char* w = gen::kWords[n->ref].name;
      snprintf(buf, cap, "(%s", strcmp(w, "WAIT UNTIL") == 0 ? "WAIT-UNTIL" : w);
      return;
    }
    case NodeKind::kUnary: case NodeKind::kBinary:
      snprintf(buf, cap, "(%s", kOpNames[static_cast<uint8_t>(n->op)]);
      return;
    default: leafText(c, n, buf, cap); return;
  }
}

}  // namespace

void printReads(const Node* program, const char* src, const TokenList& toks, const Parser& p, char* out,
                size_t cap) {
  Out o(out, cap);
  if (program == nullptr) return;
  const Ctx c{src, toks, p};
  struct Item {
    const Node* n;
    const Node* child;
    bool opened;
  };
  Item st[128];
  uint8_t ns = 0;
  st[ns++] = {program, nullptr, false};
  char buf[64];
  while (ns > 0) {
    Item& it = st[ns - 1];
    if (!it.opened) {
      it.opened = true;
      it.child = it.n->first;
      openText(c, it.n, buf, sizeof buf);
      if (buf[0] != '\0') o.put(buf);
      continue;
    }
    if (it.child != nullptr) {
      const Node* ch = it.child;
      it.child = ch->next;
      if (ns < 128) st[ns++] = {ch, nullptr, false};
      continue;
    }
    const char* close = kClosers[static_cast<uint8_t>(it.n->kind)];
    if (close[0] != '\0') o.put(close);
    --ns;
  }
}

}  // namespace logo
}  // namespace cq
