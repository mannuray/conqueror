#include "logo/vm.h"

#include <stdio.h>

#include "logo/vm_ops.h"

namespace cq {
namespace logo {

void Vm::load(const Program& p, uint32_t seed) {
  m_p = &p;
  m_pc = 0;
  m_line = 0;
  m_sensor_read = false;
  m_rng = seed != 0 ? seed : 1;
  m_executed = 0;
  m_signal = m_done = VmStatus::kYield;
  m_sp = 0;
  m_nl = 0;
  m_nf = 1;
  m_frames[0] = Frame{0, 0, -1, 0};
  m_error = RunError();
}

VmStatus Vm::run(uint16_t budget, const Sensors& s) {
  if (m_done != VmStatus::kYield || m_p == nullptr) return m_done;
  m_sensors = &s;
  for (uint16_t i = 0; i < budget; ++i) {
    const Instr& in = m_p->code[m_pc++];
    ++m_executed;
    VmOps::run(*this, in);
    if (m_signal == VmStatus::kYield) continue;
    const VmStatus out = m_signal;
    m_signal = VmStatus::kYield;
    return out;
  }
  return VmStatus::kYield;
}

int32_t Vm::round() const { return m_nl > 0 ? m_loops[m_nl - 1].round : 0; }

bool Vm::push(Value v) {
  if (m_sp >= kMaxValues) {
    error(7014);
    return false;
  }
  m_stack[m_sp++] = v;
  return true;
}

void Vm::error(uint16_t code, const DiagArg* a, uint8_t n) {
  m_error.code = code;
  m_error.line = m_line;
  renderMessage(errorTemplate(code), a, n, m_error.msg, sizeof m_error.msg);
  m_done = m_signal = VmStatus::kError;
}

void Vm::fail(uint16_t code, const DiagArg* a, uint8_t n) { error(code, a, n); }

// 7012: "{word} needs {type}, but got {value}."
void Vm::typeError(const char* word, const char* type, Value got) {
  char v[48];
  valueText(*m_p, got, true, v, sizeof v);
  const DiagArg a[] = {{"word", word}, {"type", type}, {"value", v}};
  error(7012, a, 3);
}

void Vm::leaveFrame() {
  const Frame& f = m_frames[--m_nf];
  m_sp = f.args_base;
  m_nl = f.loop_base;
  m_pc = f.ret_pc;
}

}  // namespace logo
}  // namespace cq
