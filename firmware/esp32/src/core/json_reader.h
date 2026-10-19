// A tiny pull reader for the flat JSON the pages send (bridge LLD R2.9, R15.6). Pure, host-tested via ws_codec.
#pragma once

#include <stddef.h>

namespace cq {
namespace core {

class Reader {
 public:
  Reader(const char* s, size_t n) : m_s(s), m_end(s + n) {}
  bool cut() const { return m_cut; }  // the last str() did not fit and was truncated
  void ws();
  bool eat(char c);
  bool peek(char c);
  bool str(char* out, size_t cap);              // truncated to cap-1; escapes kept only for \" and \\.
  bool num(double& value, char* raw, size_t cap);  // raw (for t0): re-formatted as a valid JSON number (review M5)
  bool word(const char* w);
  bool skip();                                   // any value, including nested objects / arrays
  bool atEnd();

 private:
  bool skipNested();
  const char* m_s;
  const char* m_end;
  bool m_cut = false;
};

}  // namespace core
}  // namespace cq
