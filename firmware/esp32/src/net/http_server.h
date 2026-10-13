// esp_http_server on port 80 (bridge LLD R2.6 HttpServer): WebSocket /ws here; the page and
// assets (Task 7) and /info, /help, /source.tar.gz (Task 9) register their own URIs.
// WS frames are received in the httpd task and posted to link_task; link_task's replies are
// queued and sent by the httpd task (httpd_queue_work), never from another task directly.
#pragma once

#include <esp_http_server.h>
#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

bool httpServerStart();
httpd_handle_t httpServer();
bool wsSend(int fd, const char* json, size_t len);  // false: out queue full (counted)
void wsClose(int fd);                               // reap / OTA: close a WS session
uint32_t wsDropped();

}  // namespace net
}  // namespace cq
