// Port: replies to WebSocket clients (bridge LLD R15.3). Real: net/ports_esp32.cpp; host: fakes/.
#pragma once

#include <stddef.h>

namespace cq {
namespace ports {

bool clientSend(int fd, const char* json, size_t len);  // false: not queued (full / too big) - the caller logs
bool clientClose(int fd);                                // false: no live WebSocket session on fd

}  // namespace ports
}  // namespace cq
