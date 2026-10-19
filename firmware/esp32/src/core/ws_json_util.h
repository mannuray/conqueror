// Shared helpers of the ws_json builders (bridge LLD R15.7). Internal to core/ws_json_*.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/motion_service.h"

namespace cq {
namespace core {
namespace detail {

size_t fmt(char* out, size_t cap, const char* f, ...) __attribute__((format(printf, 3, 4)));  // 0 when it does not fit
inline bool bit(uint32_t v, unsigned b) { return ((v >> b) & 1u) != 0; }
inline const char* tf(bool v) { return v ? "true" : "false"; }
const char* primName(PrimKind k);

}  // namespace detail
}  // namespace core
}  // namespace cq
