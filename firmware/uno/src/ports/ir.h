#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace ir {
void begin();
bool popEdge(uint32_t& t_us);  // falling-edge timestamps captured by the ISR (16-entry ring)
}  // namespace ir
}  // namespace port
}  // namespace cq
