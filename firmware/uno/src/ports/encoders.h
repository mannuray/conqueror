#pragma once
#include "types/hw_types.h"

namespace cq {
namespace port {
namespace encoders {
void begin();
void snapshot(EncoderSnapshot& out);  // atomic copy; counts are raw rising edges (unsigned)
}  // namespace encoders
}  // namespace port
}  // namespace cq
