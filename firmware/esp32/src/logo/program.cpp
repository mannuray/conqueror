#include "logo/program.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace logo {
namespace {

constexpr const char* kEffectWords[] = {"FORWARD", "BACK",  "RIGHT", "LEFT", "SPEED", "WAIT",       "LED",       "BEEP",
                                        "SOUND",   "LOOK",  "HOME",  "ARC",  "PIVOT", "PLAY",       "SAY",
                                        "WAIT UNTIL", "WAIT UNTIL", "GO", "HOLD"};
static_assert(sizeof kEffectWords / sizeof kEffectWords[0] == static_cast<size_t>(EffectKind::kCount), "one per kind");

constexpr uint8_t kMaxDepth = 64;

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

class Text {
 public:
  Text(char* out, size_t cap) : m_out(out), m_cap(cap) { m_out[0] = '\0'; }
  void put(const char* s, size_t n) {
    for (size_t i = 0; i < n && m_len + 1 < m_cap; ++i) m_out[m_len++] = s[i];
    m_out[m_len] = '\0';
  }
  void put(const char* s) { put(s, strlen(s)); }
  void space() {
    if (m_len > 0 && m_out[m_len - 1] != '[') put(" ");
  }

 private:
  char* m_out;
  size_t m_cap;
  size_t m_len = 0;
};

void putItem(const Program& p, Value v, Text& t) {
  char buf[16];
  uint16_t len = 0;
  switch (v.tag) {
    case VTag::kNum: formatMilli(v.n, buf, sizeof buf); t.put(buf); return;
    case VTag::kBool: t.put(v.n != 0 ? "TRUE" : "FALSE"); return;
    case VTag::kWord: {
      const char* s = p.wordText(static_cast<uint16_t>(v.n), len);
      t.put(s, len);
      return;
    }
    case VTag::kList: return;  // handled by the caller
  }
}

bool sameWord(const Program& p, int32_t a, int32_t b) {
  uint16_t la = 0, lb = 0;
  const char* sa = p.wordText(static_cast<uint16_t>(a), la);
  const char* sb = p.wordText(static_cast<uint16_t>(b), lb);
  if (la != lb) return false;
  for (uint16_t i = 0; i < la; ++i) {
    if (upper(sa[i]) != upper(sb[i])) return false;
  }
  return true;
}

bool sameScalar(const Program& p, Value a, Value b) {
  if (a.tag != b.tag) return false;
  if (a.tag == VTag::kWord) return sameWord(p, a.n, b.n);
  return a.n == b.n;
}

}  // namespace

const char* effectWord(EffectKind k) { return kEffectWords[static_cast<uint8_t>(k)]; }

void Program::clear() {
  n_code = n_consts = n_items = n_text = 0;
  n_procs = 0;
  handles_obstacles = false;
  ok = false;
}

int16_t Program::addWord(const char* s, uint16_t len) {
  if (n_consts >= kMaxConsts || n_text + len > kTextBytes) return -1;
  memcpy(text + n_text, s, len);
  consts[n_consts] = Const{VTag::kWord, n_text, len};
  n_text = static_cast<uint16_t>(n_text + len);
  return static_cast<int16_t>(n_consts++);
}

int16_t Program::addList(const Value* v, uint16_t n) {
  if (n_consts >= kMaxConsts || n_items + n > kMaxItems) return -1;
  memcpy(items + n_items, v, n * sizeof(Value));
  consts[n_consts] = Const{VTag::kList, n_items, n};
  n_items = static_cast<uint16_t>(n_items + n);
  return static_cast<int16_t>(n_consts++);
}

const char* Program::wordText(uint16_t c, uint16_t& len) const {
  len = consts[c].len;
  return text + consts[c].start;
}

void formatMilli(int32_t milli, char* out, size_t cap) {
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

void valueText(const Program& p, Value v, bool brackets, char* out, size_t cap) {
  Text t(out, cap);
  if (v.tag != VTag::kList) return putItem(p, v, t);
  struct Pos {
    uint16_t list;
    uint16_t i;
  };
  Pos st[kMaxDepth];
  uint8_t ns = 0;
  st[ns++] = {static_cast<uint16_t>(v.n), 0};
  if (brackets) t.put("[");
  while (ns > 0) {
    Pos& top = st[ns - 1];
    const Const& c = p.consts[top.list];
    if (top.i >= c.len) {
      if (ns > 1 || brackets) t.put("]");
      --ns;
      continue;
    }
    const Value item = p.items[c.start + top.i++];
    t.space();
    if (item.tag != VTag::kList) {
      putItem(p, item, t);
    } else if (ns < kMaxDepth) {
      t.put("[");
      st[ns++] = {static_cast<uint16_t>(item.n), 0};
    } else {
      t.put("[...]");
    }
  }
}

bool sameValue(const Program& p, Value a, Value b) {
  if (a.tag != VTag::kList || b.tag != VTag::kList) return sameScalar(p, a, b);
  struct Pair {
    uint16_t a;
    uint16_t b;
    uint16_t i;
  };
  Pair st[kMaxDepth];
  uint8_t ns = 0;
  st[ns++] = {static_cast<uint16_t>(a.n), static_cast<uint16_t>(b.n), 0};
  while (ns > 0) {
    Pair& top = st[ns - 1];
    const Const& ca = p.consts[top.a];
    const Const& cb = p.consts[top.b];
    if (ca.len != cb.len) return false;
    if (top.i >= ca.len) {
      --ns;
      continue;
    }
    const Value x = p.items[ca.start + top.i];
    const Value y = p.items[cb.start + top.i++];
    if (x.tag == VTag::kList && y.tag == VTag::kList && ns < kMaxDepth) {
      st[ns++] = {static_cast<uint16_t>(x.n), static_cast<uint16_t>(y.n), 0};
    } else if (!sameScalar(p, x, y)) {
      return false;
    }
  }
  return true;
}

}  // namespace logo
}  // namespace cq
