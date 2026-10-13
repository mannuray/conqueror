// Boot diagnostics (design-notes §38): boot counter + last reset reasons kept in NVS.
#pragma once

#include <stddef.h>

namespace cq {
namespace net {

void bootDiagBegin();                          // once, first thing in setup(): count + log
size_t bootDiagJson(char* out, size_t cap);    // {"count":N,"reason":"...","recent":[...]}

}  // namespace net
}  // namespace cq
