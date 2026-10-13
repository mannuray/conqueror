// USB-CDC status log ("[  12345] text"). Safe from any task; never logs secrets.
#pragma once

namespace cq {
namespace net {

void logBegin();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace net
}  // namespace cq
