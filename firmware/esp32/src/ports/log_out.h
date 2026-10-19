// Port: the status log (bridge LLD R15.3). Never pass secrets.
#pragma once

namespace cq {
namespace ports {

void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace ports
}  // namespace cq
