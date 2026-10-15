// MJPEG on port 81 /stream, max 2 viewers (bridge LLD R2.6 StreamServer, R2.14 #6). A separate
// httpd instance: a stream blocks only its own socket task, never port 80.
#pragma once

#include <esp_http_server.h>

#include <stdint.h>

namespace cq {
namespace net {

bool streamServerStart();
httpd_handle_t streamServerHandle();  // port 81 server (null before start / on failure)
uint8_t streamViewers();

}  // namespace net
}  // namespace cq
