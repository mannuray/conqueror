#include "logo/diag.h"

#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

const char* findArg(const DiagArg* args, uint8_t n, const char* key, size_t klen) {
  for (uint8_t i = 0; i < n; ++i) {
    if (strlen(args[i].key) == klen && strncmp(args[i].key, key, klen) == 0) return args[i].value;
  }
  return "";
}

bool sentenceEnd(const char* p) { return (*p == '.' || *p == '?' || *p == '!') && (p[1] == ' ' || p[1] == '\0'); }

size_t cutBack(const char* out, size_t from) {  // back to the start of the dropped sentence and the spaces before it
  size_t o = from;
  while (o > 0 && out[o - 1] == ' ') --o;
  return o;
}

size_t appendText(const char* v, char* out, size_t o, size_t cap) {
  for (; *v != '\0' && o + 1 < cap; ++v) out[o++] = *v;
  return o;
}

// The "{key}" at p: its value appended, p moved to the '}', drop set when the value is empty. False: no '}'.
bool placeholder(const char*& p, const DiagArg* args, uint8_t n, char* out, size_t& o, size_t cap, bool& drop) {
  const char* close = strchr(p, '}');
  if (close == nullptr) return false;
  const char* v = findArg(args, n, p + 1, static_cast<size_t>(close - p - 1));
  if (*v == '\0') drop = true;
  o = appendText(v, out, o, cap);
  p = close;
  return true;
}

}  // namespace

void renderMessage(const char* tmpl, const DiagArg* args, uint8_t n, char* out, size_t cap) {
  size_t o = 0, sentence = 0;
  bool drop = false;
  for (const char* p = tmpl; *p != '\0'; ++p) {
    if (*p == '{') {
      if (!placeholder(p, args, n, out, o, cap, drop)) break;
      continue;
    }
    if (o + 1 < cap) out[o++] = *p;
    if (!sentenceEnd(p)) continue;
    if (drop) o = cutBack(out, sentence);
    drop = false;
    sentence = o + 1;  // after the following space
  }
  if (drop) o = cutBack(out, sentence > 0 ? sentence - 1 : 0);
  out[o < cap ? o : cap - 1] = '\0';
}

const char* errorTemplate(uint16_t code) {
  for (uint8_t i = 0; i < gen::kErrorCount; ++i) {
    if (gen::kErrors[i].code == code) return gen::kErrors[i].text;
  }
  return "";
}

void Diagnostics::add(uint16_t code, uint16_t line, uint16_t col, const DiagArg* args, uint8_t n, bool warning) {
  m_last = -1;
  for (uint8_t i = 0; i < m_n; ++i) {
    if (m_d[i].code == code && m_d[i].line == line && m_d[i].col == col) {
      m_last = static_cast<int8_t>(i);  // a duplicate: a hint added after it goes to the first one
      return;
    }
  }
  if (m_n == kCap) return;
  Diag& d = m_d[m_n++];
  d.code = code;
  d.line = line;
  d.col = col;
  d.warning = warning;
  renderMessage(errorTemplate(code), args, n, d.msg, sizeof d.msg);
  m_last = static_cast<int8_t>(m_n - 1);
}

void Diagnostics::appendToLast(const char* text) {
  if (m_last < 0) return;
  Diag& d = m_d[m_last];
  if (strstr(d.msg, text) != nullptr) return;  // the same hint once (a duplicate error adds it again)
  strncat(d.msg, text, sizeof d.msg - strlen(d.msg) - 1);
}

uint8_t Diagnostics::errorCount() const {
  uint8_t c = 0;
  for (uint8_t i = 0; i < m_n; ++i) c += m_d[i].warning ? 0 : 1;
  return c;
}

const Diag* Diagnostics::first() const {
  const Diag* best = nullptr;
  for (uint8_t i = 0; i < m_n; ++i) {
    const Diag& d = m_d[i];
    if (d.warning) continue;
    if (best == nullptr || d.line < best->line || (d.line == best->line && d.col < best->col)) best = &d;
  }
  return best;
}

}  // namespace logo
}  // namespace cq
