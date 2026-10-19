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
struct BigMsg {  // Plan O tank check: the log / tank replies are larger than an OutMsg (they were dropped)
  int fd;
  uint16_t len;
  char data[core::kWsBigMax];
};

httpd_handle_t g_server = nullptr;
QueueHandle_t g_out = nullptr;
QueueHandle_t g_out_big = nullptr;  // 2 slots
std::atomic<bool> g_work_pending{false};
std::atomic<uint32_t> g_dropped{0};

void queueSendWork();

// Review I2: the httpd task also receives every drive/stop, so a send pass is bounded and a client
// whose send fails (sleeping phone, full socket buffer) is closed instead of blocking again.
struct SendPass {
  int failed[4];
  int n_failed = 0;
  void send(int fd, char* data, uint16_t len) {
    for (int i = 0; i < n_failed; ++i) {
      if (failed[i] == fd) {
        ++g_dropped;
        return;
      }
    }
    if (httpd_ws_get_fd_info(g_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      ++g_dropped;  // review M6: closed meanwhile, or the fd now belongs to another session
      return;
    }
    httpd_ws_frame_t f{};
    f.final = true;
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = reinterpret_cast<uint8_t*>(data);
    f.len = len;
    if (httpd_ws_send_frame_async(g_server, fd, &f) != ESP_OK) {
      ++g_dropped;
      if (n_failed < 4) failed[n_failed++] = fd;
      httpd_sess_trigger_close(g_server, fd);  // kWsClose follows: link_task drops the client
    }
  }
};

void sendWork(void*) {
  g_work_pending = false;  // cleared first: a message posted while we drain queues new work
  static OutMsg m;         // httpd task only
  static BigMsg big;
  constexpr int kMaxPerPass = 16;
  SendPass pass;
  int sent = 0;
  while (sent < kMaxPerPass && xQueueReceive(g_out, &m, 0) == pdTRUE) {
    ++sent;
    pass.send(m.fd, m.data, m.len);
  }
  if (xQueueReceive(g_out_big, &big, 0) == pdTRUE) pass.send(big.fd, big.data, big.len);  // one big reply per pass
  if (uxQueueMessagesWaiting(g_out) > 0 || uxQueueMessagesWaiting(g_out_big) > 0) queueSendWork();  // rest next pass
}

void queueSendWork() {
  bool expected = false;
  if (g_work_pending.compare_exchange_strong(expected, true) &&
      httpd_queue_work(g_server, sendWork, nullptr) != ESP_OK) {
    g_work_pending = false;
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

// The frame text into the link command; review M7: too long for link_q - a stop must still stop, so it is forwarded
// as a short stop (same id); anything else gets len 0 and link_task answers nack INVALID_COMMAND.
void copyText(LinkCommand& cmd, const uint8_t* buf, size_t len) {
  if (len < sizeof(cmd.text)) {
    memcpy(cmd.text, buf, len);
    cmd.text[len] = '\0';
    cmd.len = static_cast<uint16_t>(len);
    return;
  }
  const core::WsCommand big = core::parseWsCommand(reinterpret_cast<const char*>(buf), len);
  if (big.kind != core::WsCmdKind::kStop) return;
  const int n = big.has_id ? snprintf(cmd.text, sizeof(cmd.text), "{\"t\":\"stop\",\"id\":%ld}", static_cast<long>(big.id))
                           : snprintf(cmd.text, sizeof(cmd.text), "{\"t\":\"stop\"}");
  cmd.len = static_cast<uint16_t>(n);
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
  copyText(cmd, buf, f.len);
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
  // Review M6: a lost kWsClose would leave a ghost client whose fd number gets reused. linkPost
  // waits 100 ms per try; link_task drains every 2 ms, so this only loops if link_task is stuck.
  bool posted = false;
  for (int i = 0; i < 10 && !posted; ++i) posted = linkPost(cmd, false);
  if (!posted) logf("ws: close of fd %d not delivered to link_task", fd);
  close(fd);  // with a close_fn set, the application closes the socket
}

}  // namespace

bool httpServerStart() {
  g_out = xQueueCreate(24, sizeof(OutMsg));  // allocated once at startup
  g_out_big = xQueueCreate(2, sizeof(BigMsg));  // Plan O: ~7 KB for the log / tank replies
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = board::kHttpPort;
  cfg.ctrl_port = 32768;
  cfg.max_open_sockets = 6;  // 4 WS + 2 page loads; lwIP budget: Task 1 Step 6
  cfg.max_uri_handlers = 20;  // Plan N3: + 5 Tank Logo handlers
  cfg.uri_match_fn = httpd_uri_match_wildcard;  // /logo/programs/<name>
  cfg.lru_purge_enable = true;
  cfg.stack_size = 6144;
  cfg.core_id = 0;
  cfg.close_fn = onSockClose;
  cfg.send_wait_timeout = 1;  // s (default 5): a stuck client must not stall drive/stop reception (I2)
  cfg.recv_wait_timeout = 1;
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
  if (g_server == nullptr || len == 0 || len > core::kWsBigMax) return false;
  if (len > core::kWsJsonMax) {  // Plan O: a log / tank reply
    static BigMsg b;  // link_task only
    b.fd = fd;
    b.len = static_cast<uint16_t>(len);
    memcpy(b.data, json, len);
    if (xQueueSendToBack(g_out_big, &b, 0) != pdTRUE) {
      ++g_dropped;
      return false;
    }
    queueSendWork();
    return true;
  }
  static OutMsg m;  // link_task only
  m.fd = fd;
  m.len = static_cast<uint16_t>(len);
  memcpy(m.data, json, len);
  if (xQueueSendToBack(g_out, &m, 0) != pdTRUE) {
    ++g_dropped;
    return false;
  }
  queueSendWork();
  return true;
}

bool wsClose(int fd) {
  // Only a live WebSocket session (review M6: never close an unrelated session that reused the fd).
  if (g_server == nullptr || httpd_ws_get_fd_info(g_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
    return false;
  }
  return httpd_sess_trigger_close(g_server, fd) == ESP_OK;
}

uint32_t wsDropped() { return g_dropped; }

}  // namespace net
}  // namespace cq
