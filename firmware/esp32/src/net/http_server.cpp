#include "net/http_server.h"

#include <Arduino.h>
#include <lwip/sockets.h>
#include <string.h>

#include <atomic>

#include "board_config.h"
#include "core/ws_codec.h"
#include "core/ws_json.h"
#include "net/link_task.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

struct OutMsg {
  int fd;
  uint16_t len;
  char data[core::kWsJsonMax];
};

httpd_handle_t g_server = nullptr;
QueueHandle_t g_out = nullptr;
std::atomic<bool> g_work_pending{false};
std::atomic<uint32_t> g_dropped{0};

void sendWork(void*) {
  g_work_pending = false;  // cleared first: a message posted while we drain queues new work
  static OutMsg m;         // httpd task only
  while (xQueueReceive(g_out, &m, 0) == pdTRUE) {
    httpd_ws_frame_t f{};
    f.final = true;
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = reinterpret_cast<uint8_t*>(m.data);
    f.len = m.len;
    if (httpd_ws_send_frame_async(g_server, m.fd, &f) != ESP_OK) ++g_dropped;
  }
}

// One marker per WebSocket session: set when link_task has been told about the client.
uint8_t g_ws_open_marker = 0;
void noFree(void*) {}  // the marker is static; httpd must not free it

void postWsOpen(httpd_req_t* req, int fd) {
  LinkCommand cmd{};
  cmd.kind = LinkCommand::Kind::kWsOpen;
  cmd.fd = fd;
  linkPost(cmd, false);
  req->sess_ctx = &g_ws_open_marker;
  req->free_ctx = noFree;
}

esp_err_t wsHandler(httpd_req_t* req) {
  const int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET) {  // handshake (only if the IDF build calls the handler for it)
    postWsOpen(req, fd);
    return ESP_OK;
  }
  // Arduino-ESP32 3.3 / IDF 5.5 does not call the handler for the handshake (and its post-handshake
  // callback is compiled out), so a client is announced to link_task on its first frame. The page
  // sends `hello` right after connecting, so it is registered before any drive (bench 2026-10-13).
  if (req->sess_ctx == nullptr) postWsOpen(req, fd);
  static uint8_t buf[512];  // httpd task only
  httpd_ws_frame_t f{};
  esp_err_t err = httpd_ws_recv_frame(req, &f, 0);  // length first
  if (err != ESP_OK) return err;
  if (f.len >= sizeof(buf)) return ESP_FAIL;  // oversized: httpd closes the socket
  f.payload = buf;
  err = httpd_ws_recv_frame(req, &f, f.len);
  if (err != ESP_OK) return err;
  if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
  static uint32_t s_post_seq = 0;  // httpd task only: one running number for all WS texts
  LinkCommand cmd{};
  cmd.kind = LinkCommand::Kind::kWsText;
  cmd.fd = fd;
  cmd.post_seq = ++s_post_seq;
  if (f.len < sizeof(cmd.text)) {
    memcpy(cmd.text, buf, f.len);
    cmd.text[f.len] = '\0';
    cmd.len = static_cast<uint16_t>(f.len);
  }  // else len 0: link_task answers nack INVALID_COMMAND
  // STOP fast path (R2.11): `stop` jumps to the queue front. link_task's StopBarrier drops any
  // drive / cam_joy posted before it, so an older drive can never restart the tank after it.
  const bool stop = core::parseWsCommand(cmd.text, cmd.len).kind == core::WsCmdKind::kStop;
  if (!linkPost(cmd, stop)) ++g_dropped;
  return ESP_OK;
}

void onSockClose(httpd_handle_t, int fd) {
  LinkCommand cmd{};
  cmd.kind = LinkCommand::Kind::kWsClose;  // link_task ignores sockets that were plain HTTP
  cmd.fd = fd;
  linkPost(cmd, false);
  close(fd);  // with a close_fn set, the application closes the socket
}

}  // namespace

bool httpServerStart() {
  g_out = xQueueCreate(24, sizeof(OutMsg));  // allocated once at startup
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = board::kHttpPort;
  cfg.ctrl_port = 32768;
  cfg.max_open_sockets = 6;  // 4 WS + 2 page loads; lwIP budget: Task 1 Step 6
  cfg.max_uri_handlers = 12;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 6144;
  cfg.core_id = 0;
  cfg.close_fn = onSockClose;
  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    logf("http: start failed");
    return false;
  }
  httpd_uri_t ws{};
  ws.uri = "/ws";
  ws.method = HTTP_GET;
  ws.handler = wsHandler;
  ws.is_websocket = true;
  httpd_register_uri_handler(g_server, &ws);
  logf("http: port %u, WebSocket /ws", board::kHttpPort);
  return true;
}

httpd_handle_t httpServer() { return g_server; }

bool wsSend(int fd, const char* json, size_t len) {
  if (g_server == nullptr || len == 0 || len > core::kWsJsonMax) return false;
  static OutMsg m;  // link_task only
  m.fd = fd;
  m.len = static_cast<uint16_t>(len);
  memcpy(m.data, json, len);
  if (xQueueSendToBack(g_out, &m, 0) != pdTRUE) {
    ++g_dropped;
    return false;
  }
  bool expected = false;
  if (g_work_pending.compare_exchange_strong(expected, true) &&
      httpd_queue_work(g_server, sendWork, nullptr) != ESP_OK) {
    g_work_pending = false;
  }
  return true;
}

void wsClose(int fd) {
  if (g_server != nullptr) httpd_sess_trigger_close(g_server, fd);
}

uint32_t wsDropped() { return g_dropped; }

}  // namespace net
}  // namespace cq
