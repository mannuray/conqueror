// Port: replies to WebSocket clients (bridge LLD R15.3). Real: net/ports_esp32.cpp; host: fakes/.
#pragma once

#include <stddef.h>

namespace cq {
namespace ports {

bool clientSend(int fd, const char* json, size_t len);  // false: not queued (full / too big) - the caller logs
bool clientClose(int fd);
// Plan Z2b-1 (R27.1): the dotted IPv4 address the socket comes from (getpeername; a v4-mapped v6 too). False: not known.
bool clientPeerIp(int fd, char* out, size_t cap);                                // false: no live WebSocket session on fd

}  // namespace ports
}  // namespace cq
