// The checked program waiting to be run (Tank Logo LLD §20.1): the last check's compiled program, its number and
// name. The worker puts every check here; logo_run takes a copy only when the number is the current one and the
// program compiled OK - so the tank runs exactly what the page saw. Pure; the mutex around it lives in net/.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/program.h"

namespace cq {
namespace logo {

class CheckBox {
 public:
  static constexpr size_t kNameCap = 25;
  enum class Take : uint8_t { kOk, kCheckAgain };

  explicit CheckBox(Program& checked) : m_p(checked) {}
  // A new check: its program (copied when it compiled), a saved program's name or "". Returns its number (1, 2 …).
  uint32_t put(const Program* compiled, const char* name);
  Take take(uint32_t prog, Program& out, char* name, size_t cap) const;
  uint32_t current() const { return m_prog; }

 private:
  Program& m_p;
  uint32_t m_prog = 0;
  bool m_ok = false;
  char m_name[kNameCap] = "";
};

}  // namespace logo
}  // namespace cq
