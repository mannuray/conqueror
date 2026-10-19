#include "core/json_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace cq {
namespace core {
namespace {
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isNumChar(char c) { return strchr("+-.eE", c) != nullptr || (c >= '0' && c <= '9'); }
}  // namespace

void Reader::ws() {
  while (m_s < m_end && isSpace(*m_s)) ++m_s;
}

bool Reader::eat(char c) {
  ws();
  if (m_s < m_end && *m_s == c) {
    ++m_s;
    return true;
  }
  return false;
}

bool Reader::peek(char c) {
  ws();
  return m_s < m_end && *m_s == c;
}

bool Reader::str(char* out, size_t cap) {
  if (!eat('"')) return false;
  size_t n = 0;
  m_cut = false;
  while (m_s < m_end && *m_s != '"') {
    char c = *m_s++;
    if (c == '\\') {
      if (m_s >= m_end) return false;
      c = *m_s++;
    }
    if (n + 1 < cap) out[n++] = c;
    else m_cut = true;
  }
  if (cap) out[n] = '\0';
  return eat('"');
}

bool Reader::num(double& value, char* raw, size_t cap) {
  ws();
  const char* start = m_s;
  while (m_s < m_end && isNumChar(*m_s)) ++m_s;
  const size_t n = static_cast<size_t>(m_s - start);
  if (n == 0 || n >= 24) return false;
  char tmp[24];
  memcpy(tmp, start, n);
  tmp[n] = '\0';
  char* endp = nullptr;
  value = strtod(tmp, &endp);
  if (endp != tmp + n || !isfinite(value)) return false;
  if (raw && cap > 0) snprintf(raw, cap, "%.15g", value);
  return true;
}

bool Reader::word(const char* w) {
  ws();
  const size_t n = strlen(w);
  if (static_cast<size_t>(m_end - m_s) < n || strncmp(m_s, w, n) != 0) return false;
  m_s += n;
  return true;
}

bool Reader::skipNested() {  // at '{' or '[': to the matching close, strings skipped whole
  int depth = 0;
  bool in_str = false;
  while (m_s < m_end) {
    const char c = *m_s++;
    if (in_str) {
      if (c == '\\') ++m_s;  // an escaped character: skipped, still inside the string
      else in_str = c != '"';
    } else if (c == '"') {
      in_str = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if ((c == '}' || c == ']') && --depth == 0) {
      return true;
    }
  }
  return false;
}

bool Reader::skip() {
  ws();
  if (m_s >= m_end) return false;
  if (*m_s == '"') {
    char dummy[2];
    return str(dummy, sizeof(dummy));
  }
  if (*m_s == '{' || *m_s == '[') return skipNested();
  if (word("true") || word("false") || word("null")) return true;
  double v = 0;
  return num(v, nullptr, 0);
}

bool Reader::atEnd() {
  ws();
  return m_s == m_end;
}

}  // namespace core
}  // namespace cq
