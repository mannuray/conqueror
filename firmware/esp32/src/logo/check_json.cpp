#include "logo/check_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace cq {
namespace logo {
namespace {

void diags(const Diagnostics& d, JsonOut& out) {
  out.put("\"diags\":[");
  for (uint8_t i = 0; i < d.count(); ++i) {
    const Diag& x = d.at(i);
    out.putf("%s{\"code\":%u,\"line\":%u,\"col\":%u,\"msg\":", i > 0 ? "," : "", x.code, x.line, x.col);
    out.text(x.msg, strlen(x.msg));
    out.putf(",\"warning\":%s}", x.warning ? "true" : "false");
  }
  out.put("],");
}

// Your words: name (as written, upper case), inputs, gives a value.
void words(const CheckSpace& s, const char* src, JsonOut& out) {
  out.put("\"words\":[");
  bool first = true;
  for (uint8_t i = 0; i < s.parser.procCount(); ++i) {
    const ProcInfo& p = s.parser.proc(i);
    if (!p.valid) continue;
    char name[40];
    tokenText(src, s.toks.t[p.name_tok], name, sizeof name);
    for (char* c = name; *c != '\0'; ++c) *c = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 32) : *c;
    out.put(first ? "{\"name\":" : ",{\"name\":");
    out.text(name, strlen(name));
    out.putf(",\"inputs\":%u,\"gives\":%s}", p.n_inputs, p.gives_value ? "true" : "false");
    first = false;
  }
  out.put("],");
}

void path(const CheckSpace& s, JsonOut& out) {
  out.put("\"path\":[");
  for (uint16_t i = 0; i < s.path.n; ++i) {
    const PathPoint& p = s.path.pts[i];
    out.putf("%s[%ld,%ld,%d,%u,%u]", i > 0 ? "," : "", static_cast<long>(p.x_mm), static_cast<long>(p.y_mm),
             p.heading_deg, p.line, p.uncertain ? 1u : 0u);
  }
  out.put("],");
}

void simError(const SimResult& r, JsonOut& out) {
  if (r.code == 0) {
    out.put("\"sim_error\":null");
    return;
  }
  out.putf("\"sim_error\":{\"code\":%u,\"line\":%u,\"msg\":", r.code, r.line);
  out.text(r.msg, strlen(r.msg));
  out.put("}");
}

}  // namespace

void JsonOut::raw(const char* s, size_t n) {
  while (n > 0) {
    if (m_n == sizeof m_buf) {
      m_ok = m_flush(m_ctx, m_buf, m_n) && m_ok;
      m_n = 0;
    }
    const size_t k = n < sizeof m_buf - m_n ? n : sizeof m_buf - m_n;
    memcpy(m_buf + m_n, s, k);
    m_n += k;
    s += k;
    n -= k;
  }
}

void JsonOut::put(const char* s) { raw(s, strlen(s)); }

void JsonOut::putf(const char* fmt, ...) {
  char tmp[96];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) raw(tmp, static_cast<size_t>(n) < sizeof tmp ? static_cast<size_t>(n) : sizeof tmp - 1);
}

void JsonOut::text(const char* s, size_t n) {
  raw("\"", 1);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char esc[8];
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = static_cast<char>(c);
      raw(esc, 2);
    } else if (c < 0x20) {
      snprintf(esc, sizeof esc, "\\u%04x", c);
      raw(esc, 6);
    } else {
      raw(s + i, 1);
    }
  }
  raw("\"", 1);
}

bool JsonOut::finish() {
  if (m_n > 0) m_ok = m_flush(m_ctx, m_buf, m_n) && m_ok;
  m_n = 0;
  return m_ok;
}

bool writeCheckReply(const CheckSpace& s, const char* src, bool runnable, uint32_t prog, JsonOut& out) {
  out.putf("{\"ok\":%s,\"prog\":%lu,", runnable ? "true" : "false", static_cast<unsigned long>(prog));
  diags(s.diags, out);
  words(s, src, out);
  out.putf("\"handles_obstacles\":%s,", s.result.handles_obstacles ? "true" : "false");
  path(s, out);
  out.putf("\"cut_short\":%s,", s.sim.cut_short ? "true" : "false");
  simError(s.sim, out);
  out.put("}");
  return out.finish();
}

}  // namespace logo
}  // namespace cq
