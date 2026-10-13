#pragma once

namespace cq {
namespace port {
namespace button {
void begin();
bool pressed();  // raw level (active low, pull-up); debounced in the domain
}  // namespace button
}  // namespace port
}  // namespace cq
