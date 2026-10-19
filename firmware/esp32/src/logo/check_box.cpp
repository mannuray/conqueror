#include "logo/check_box.h"

#include <stdio.h>

namespace cq {
namespace logo {

uint32_t CheckBox::put(const Program* compiled, const char* name) {
  m_ok = compiled != nullptr && compiled->ok;
  if (m_ok) m_p = *compiled;
  snprintf(m_name, sizeof m_name, "%s", name != nullptr ? name : "");
  return ++m_prog;
}

CheckBox::Take CheckBox::take(uint32_t prog, Program& out, char* name, size_t cap) const {
  if (prog == 0 || prog != m_prog || !m_ok) return Take::kCheckAgain;
  out = m_p;
  snprintf(name, cap, "%s", m_name);
  return Take::kOk;
}

}  // namespace logo
}  // namespace cq
