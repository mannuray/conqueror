#pragma once
#include "generated/cq_protocol.h"

namespace cq {
namespace port {
namespace watchdog {
void enable();   // 1 s
void kick();
proto::ResetReason resetReason();
}  // namespace watchdog
}  // namespace port
}  // namespace cq
