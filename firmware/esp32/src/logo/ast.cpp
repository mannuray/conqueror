#include "logo/ast.h"

namespace cq {
namespace logo {

Node* NodePool::make(NodeKind k, uint16_t tok) {
  if (m_used >= m_cap) return nullptr;
  Node* n = &m_buf[m_used++];
  *n = Node();
  n->kind = k;
  n->tok = tok;
  return n;
}

}  // namespace logo
}  // namespace cq
