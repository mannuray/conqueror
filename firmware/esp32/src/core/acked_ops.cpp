#include "core/acked_ops.h"

namespace cq {
namespace core {

bool AckedOps::push(const AckedOp& op) {
  if (m_n >= kCap) return false;
  m_ops[(m_head + m_n) % kCap] = op;
  ++m_n;
  return true;
}

bool AckedOps::front(AckedOp& out) const {
  if (m_n == 0) return false;
  out = m_ops[m_head];
  return true;
}

void AckedOps::done() {
  m_sent = false;
  if (m_n == 0) return;
  m_head = static_cast<uint8_t>((m_head + 1) % kCap);
  --m_n;
}

}  // namespace core
}  // namespace cq
