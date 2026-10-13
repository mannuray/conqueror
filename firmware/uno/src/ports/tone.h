#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace tone {
void begin();
void play(uint16_t hz);  // continuous until stop()/next play (Timer2)
void stop();
}  // namespace tone
}  // namespace port
}  // namespace cq
