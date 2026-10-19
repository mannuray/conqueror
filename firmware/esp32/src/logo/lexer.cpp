#include "logo/lexer.h"

#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isNameChar(char c) { return isLetter(c) || isDigit(c) || c == '_' || c == '#'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isDelim(char c) { return c == '\0' || isSpace(c) || c == '[' || c == ']' || c == '(' || c == ')'; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

class Lex {
 public:
  Lex(const char* s, uint16_t n, TokenList& o, Diagnostics& d) : m_s(s), m_n(n), m_o(o), m_d(d) {}
  void run();

 private:
  char at(uint16_t i) const { return i < m_n ? m_s[i] : '\0'; }
  void push(Tok k, uint16_t start, uint16_t len, uint16_t col, int32_t milli = 0, uint8_t ph = 0);
  void advance(uint16_t n);  // columns count characters: UTF-8 continuation bytes add none (LLD §4)
  uint16_t charLen(uint16_t i) const;
  void number(uint16_t start, uint16_t col);
  void tooBig(uint16_t col, uint16_t start, uint16_t len);
  bool layout(char c);  // newline, space, comment: skipped (true)
  void token(char c, uint16_t s, uint16_t col);
  bool digits(int64_t& whole);
  uint8_t fraction(int32_t& frac);
  void word(Tok kind, uint16_t s, uint16_t col);
  void quoted(uint16_t s, uint16_t col);
  bool placeholder(uint16_t s, uint16_t col);
  void minus(uint16_t s, uint16_t col);
  void symbol(char c, uint16_t s, uint16_t col);
  void badChar(uint16_t col);
  void end();

  const char* m_s;
  uint16_t m_n;
  TokenList& m_o;
  Diagnostics& m_d;
  uint16_t m_i = 0;
  uint16_t m_line = 1;
  uint16_t m_col = 1;
  bool m_full = false;
};

void Lex::push(Tok k, uint16_t start, uint16_t len, uint16_t col, int32_t milli, uint8_t ph) {
  if (m_o.n >= TokenList::kCap - 1) {  // keep the last slot for kEnd
    if (!m_full) {
      const DiagArg a[] = {{"what", "too many words"}};
      m_d.add(7010, m_line, col, a, 1);
    }
    m_full = true;
    return;
  }
  Token& t = m_o.t[m_o.n++];
  t.kind = k;
  t.start = start;
  t.len = len;
  t.line = m_line;
  t.col = col;
  t.milli = milli;
  t.ph = ph;
}

void Lex::tooBig(uint16_t col, uint16_t start, uint16_t len) {
  char tok[40];
  snprintf(tok, sizeof tok, "%.*s", len > 32 ? 32 : len, m_s + start);
  const DiagArg a[] = {{"token", tok}};
  m_d.add(7015, m_line, col, a, 1);
}

bool Lex::digits(int64_t& whole) {  // false when the whole part runs past 10^9
  bool big = false;
  while (isDigit(at(m_i))) {
    if (!big) whole = whole * 10 + (at(m_i) - '0');  // stops growing once too big: never past int64
    big = big || whole > 1000000000;
    advance(1);
  }
  return !big;
}

uint8_t Lex::fraction(int32_t& frac) {  // the decimals seen (only the first 3 count); frac in thousandths
  uint8_t decimals = 0;
  if (at(m_i) == '.' && isDigit(at(m_i + 1))) {
    advance(1);
    for (; isDigit(at(m_i)); advance(1), ++decimals) {
      if (decimals < 3) frac = frac * 10 + (at(m_i) - '0');
    }
  }
  for (uint8_t d = decimals; d < 3; ++d) frac *= 10;
  return decimals;
}

void Lex::number(uint16_t start, uint16_t col) {
  int64_t whole = 0;
  int32_t frac = 0;
  const bool fits = digits(whole);
  const uint8_t decimals = fraction(frac);
  int64_t milli = whole * 1000 + frac;
  const uint16_t len = static_cast<uint16_t>(m_i - start);
  const bool too_big = !fits || milli > gen::kNumberAbsMaxMilli;
  if (too_big || decimals > 3) tooBig(col, start, len);
  if (too_big) milli = gen::kNumberAbsMaxMilli;  // still a NUMBER token, so the parser goes on
  push(Tok::kNumber, start, len, col, static_cast<int32_t>(milli));
}

bool Lex::layout(char c) {
  if (c == '\n') {
    ++m_i;
    ++m_line;
    m_col = 1;
  } else if (isSpace(c)) {
    advance(1);
  } else if (c == ';') {
    while (m_i < m_n && at(m_i) != '\n') advance(1);
  } else {
    return false;
  }
  return true;
}

void Lex::word(Tok kind, uint16_t s, uint16_t col) {  // NAME, or :INPUT (the ':' already skipped)
  while (isNameChar(at(m_i))) advance(1);
  if (at(m_i) == '?') advance(1);
  const uint16_t len = static_cast<uint16_t>(m_i - s);
  if (kind == Tok::kName && len > gen::kNameChars) tooBig(col, s, len);
  push(kind, s, len, col);
}

void Lex::quoted(uint16_t s, uint16_t col) {  // "word - ends at a space or a bracket
  advance(1);
  while (!isDelim(at(m_i))) advance(1);
  const uint16_t len = static_cast<uint16_t>(m_i - s);
  if (len - 1 > gen::kNameChars) tooBig(col, s, len);
  push(Tok::kQuoted, s, len, col);
}

bool Lex::placeholder(uint16_t s, uint16_t col) {  // ?, ?1, ?2 (template values)
  const char n1 = at(m_i + 1);
  if ((n1 == '1' || n1 == '2') && isDelim(at(m_i + 2))) {
    advance(2);
    push(Tok::kPlaceholder, s, 2, col, 0, static_cast<uint8_t>(n1 - '0'));
    return true;
  }
  if (!isDelim(n1)) return false;
  advance(1);
  push(Tok::kPlaceholder, s, 1, col);
  return true;
}

void Lex::minus(uint16_t s, uint16_t col) {  // NEG after a space or an opening bracket and before a non-space
  const char prev = m_i == 0 ? ' ' : at(m_i - 1);
  const bool neg = (isSpace(prev) || prev == '[' || prev == '(') && m_i + 1 < m_n && !isSpace(at(m_i + 1));
  advance(1);
  push(neg ? Tok::kNeg : Tok::kMinus, s, 1, col);
}

void Lex::advance(uint16_t n) {
  for (; n > 0; --n, ++m_i) {
    if ((static_cast<uint8_t>(at(m_i)) & 0xC0) != 0x80) ++m_col;
  }
}

// Bytes in the UTF-8 character at i (1 for ASCII or a broken sequence).
uint16_t Lex::charLen(uint16_t i) const {
  const uint8_t b = static_cast<uint8_t>(at(i));
  const uint16_t want = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : (b >= 0xC0 ? 2 : 1));
  uint16_t n = 1;
  while (n < want && (static_cast<uint8_t>(at(static_cast<uint16_t>(i + n))) & 0xC0) == 0x80) ++n;
  return n;
}

void Lex::badChar(uint16_t col) {  // one 7016 for the whole character, shown as typed
  const uint16_t n = charLen(m_i);
  char ch[8];
  snprintf(ch, sizeof ch, "%.*s", static_cast<int>(n), m_s + m_i);
  char line[8];
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(m_line));
  const DiagArg a[] = {{"char", ch}, {"line", line}};
  m_d.add(7016, m_line, col, a, 2);
  advance(n);
}

void Lex::symbol(char c, uint16_t s, uint16_t col) {  // brackets and operators
  static const char kOne[] = "[]()+*/=<>";
  static const Tok kOneTok[] = {Tok::kLBracket, Tok::kRBracket, Tok::kLParen, Tok::kRParen, Tok::kPlus,
                                Tok::kStar,     Tok::kSlash,    Tok::kEq,     Tok::kLt,     Tok::kGt};
  const char* f = strchr(kOne, c);
  if (c == '\0' || f == nullptr) return badChar(col);
  Tok k = kOneTok[f - kOne];
  uint16_t len = 1;
  const char next = at(m_i + 1);
  const bool two = (c == '<' && (next == '=' || next == '>')) || (c == '>' && next == '=');
  if (two) {
    k = next == '>' ? Tok::kNe : (c == '<' ? Tok::kLe : Tok::kGe);
    len = 2;
  }
  advance(len);
  push(k, s, len, col);
}

void Lex::end() {
  Token& e = m_o.t[m_o.n++];
  e = Token();
  e.kind = Tok::kEnd;
  e.start = m_n;
  e.line = m_line;
  e.col = m_col;
}

void Lex::token(char c, uint16_t s, uint16_t col) {
  if (isDigit(c)) return number(s, col);
  if (isLetter(c)) return word(Tok::kName, s, col);
  if (c == ':' && isLetter(at(m_i + 1))) {
    advance(1);
    return word(Tok::kInput, s, col);
  }
  if (c == '"') return quoted(s, col);
  if (c == '?' && placeholder(s, col)) return;
  if (c == '-') return minus(s, col);
  symbol(c, s, col);
}

void Lex::run() {
  m_o.n = 0;
  while (m_i < m_n) {
    const char c = at(m_i);
    if (!layout(c)) token(c, m_i, m_col);
  }
  end();
}

}  // namespace

void lex(const char* src, uint16_t len, TokenList& out, Diagnostics& diags) {
  Lex l(src, len, out, diags);
  l.run();
}

bool tokenIs(const char* src, const Token& t, const char* word) {
  if (t.kind != Tok::kName || strlen(word) != t.len) return false;
  for (uint16_t i = 0; i < t.len; ++i) {
    if (upper(src[t.start + i]) != word[i]) return false;
  }
  return true;
}

void tokenText(const char* src, const Token& t, char* out, size_t cap) {
  if (t.kind == Tok::kEnd) { snprintf(out, cap, "the end"); return; }
  snprintf(out, cap, "%.*s", static_cast<int>(t.len), src + t.start);
}

}  // namespace logo
}  // namespace cq
