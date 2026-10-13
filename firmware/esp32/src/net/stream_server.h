// MJPEG on port 81 /stream, max 2 viewers (bridge LLD R2.6 StreamServer, R2.14 #6). A separate
// httpd instance: a stream blocks only its own socket task, never port 80.
#pragma once

#include <stdint.h>

namespace cq {
namespace net {

bool streamServerStart();
uint8_t streamViewers();

}  // namespace net
}  // namespace cq
