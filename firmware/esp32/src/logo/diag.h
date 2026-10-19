// Tank Logo diagnostics (LLD §5-§6, grammar.md §11): every refusal has one code, a line and a column, and a
// friendly message rendered from logo/spec/words.yaml `errors`. Pure C++, fixed capacity.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace logo {

struct DiagArg {
  const char* key;
  const char* value;
};

struct Diag {
  uint16_t code = 0;
  uint16_t line = 0;
  uint16_t col = 0;
  bool warning = false;
  char msg[192] = "";
};

// Fills {key} placeholders; a sentence whose placeholder is empty (or missing) is dropped.
void renderMessage(const char* tmpl, const DiagArg* args, uint8_t n, char* out, size_t cap);
const char* errorTemplate(uint16_t code);  // "" for an unknown code

class Diagnostics {
 public:
  static constexpr uint8_t kCap = 16;
  void add(uint16_t code, uint16_t line, uint16_t col, const DiagArg* args = nullptr, uint8_t n = 0,
           bool warning = false);
  void appendToLast(const char* text);
  uint8_t count() const { return m_n; }
  const Diag& at(uint8_t i) const { return m_d[i]; }
  uint8_t errorCount() const;
  const Diag* first() const;
  void clear() {
    m_n = 0;
    m_last = -1;
  }

 private:
  Diag m_d[kCap];
  uint8_t m_n = 0;
  int8_t m_last = -1;  // the entry the last add() made or matched (-1: none / full)
};

}  // namespace logo
}  // namespace cq
