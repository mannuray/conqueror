#include "net/stream_server.h"

#include <Arduino.h>
#include <esp_http_server.h>
#include <stdio.h>

#include <atomic>

#include "board_config.h"
#include "net/camera_service.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

constexpr const char* kBoundary = "cqframe";
// One FreeRTOS task per viewer (R3.1): stack, priority, core (core 0 with the HTTP server; the link task is on core 1).
constexpr uint32_t kViewerStack = 6144;
constexpr UBaseType_t kViewerPrio = 5;
constexpr BaseType_t kViewerCore = 0;
std::atomic<uint8_t> g_viewers{0};

void releaseViewer() {
  const uint8_t left = static_cast<uint8_t>(--g_viewers);
  logf("stream: viewer left (%u)", static_cast<unsigned>(left));
}

// R3.1: the frame loop runs in its own task (one per viewer) so the server task stays free
// for /capture and the second viewer. Owns the async request copy and the viewer slot.
void streamWorker(void* arg) {
  httpd_req_t* req = static_cast<httpd_req_t*>(arg);
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=cqframe");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char part[96];
  esp_err_t err = ESP_OK;
  while (err == ESP_OK) {
    camera_fb_t* fb = cameraGrab();
    if (fb == nullptr) break;  // camera stopped (OTA) or failed
    const int len = snprintf(part, sizeof(part),
                             "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                             kBoundary, static_cast<unsigned>(fb->len));
    err = httpd_resp_send_chunk(req, part, len);
    if (err == ESP_OK) {
      err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf),
                                  static_cast<ssize_t>(fb->len));
    }
    cameraRelease(fb);
  }
  // Review I1: complete() only hands the socket back - it is not closed (no LRU purge). Close it explicitly so the
  // page's <img> sees the end, retries, and the two stream sockets never fill up with dead streams. Order matters:
  // hand the request back first (a close queued while the async request still owns the session is lost - seen on the
  // tank: the session stayed open and port 81 stopped answering), then queue the close. complete() frees req, so the
  // handle and socket are taken before.
  httpd_handle_t server = req->handle;
  const int sockfd = httpd_req_to_sockfd(req);
  httpd_req_async_handler_complete(req);
  httpd_sess_trigger_close(server, sockfd);
  releaseViewer();
  vTaskDelete(nullptr);
}

esp_err_t refuse(httpd_req_t* req, const char* text) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_hdr(req, "Retry-After", "3");
  return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t streamHandler(httpd_req_t* req) {
  uint8_t n = g_viewers.load();
  do {
    if (!cameraReady()) return refuse(req, "camera unavailable");
    if (n >= board::kMaxStreamViewers) return refuse(req, "max 2 viewers");
  } while (!g_viewers.compare_exchange_weak(n, static_cast<uint8_t>(n + 1)));
  logf("stream: viewer joined (%u)", static_cast<unsigned>(n + 1));
  httpd_req_t* copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    releaseViewer();
    return refuse(req, "stream unavailable");
  }
  if (xTaskCreatePinnedToCore(streamWorker, "cq-stream", kViewerStack, copy, kViewerPrio, nullptr, kViewerCore) != pdPASS) {
    httpd_req_async_handler_complete(copy);
    releaseViewer();
    return refuse(req, "stream unavailable");  // the original req is still ours to answer
  }
  return ESP_OK;
}

// Plan F (spec 01 §2 /api/capture, bridge slice: no key, stream resolution): one JPEG as a download for SNAP.
// Shares port 81's two sockets with the viewers (the lwIP budget is full), so with two viewers open it is refused.
// Served while a viewer streams: the viewers' frame loops run in their own tasks (R3.1).
esp_err_t captureHandler(httpd_req_t* req) {
  camera_fb_t* fb = cameraReady() ? cameraGrab() : nullptr;
  if (fb == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Retry-After", "3");
    return httpd_resp_send(req, "camera unavailable", HTTPD_RESP_USE_STRLEN);
  }
  char disposition[64];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"tank-%lu.jpg\"",
           static_cast<unsigned long>(millis() / 1000UL));
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(fb->buf), static_cast<ssize_t>(fb->len));
  cameraRelease(fb);
  return err;
}

httpd_handle_t g_server = nullptr;  // port 81: /stream, /capture, /source.tar.gz (R3.11)

}  // namespace

bool streamServerStart() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = board::kStreamPort;
  cfg.ctrl_port = 32769;
  cfg.max_open_sockets = 2;  // = max viewers; lwIP socket budget (Task 1 Step 6)
  cfg.stack_size = 8192;
  cfg.core_id = 0;
  cfg.send_wait_timeout = 2;  // a stalled viewer is dropped, not waited for
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    logf("stream: start failed");
    return false;
  }
  g_server = server;
  httpd_uri_t u{};
  u.uri = "/stream";
  u.method = HTTP_GET;
  u.handler = streamHandler;
  httpd_register_uri_handler(server, &u);
  httpd_uri_t c{};
  c.uri = "/capture";
  c.method = HTTP_GET;
  c.handler = captureHandler;
  httpd_register_uri_handler(server, &c);
  logf("stream: http://%s.local:%u/stream (max %u viewers), /capture", board::kHostname, board::kStreamPort,
       board::kMaxStreamViewers);
  return true;
}

httpd_handle_t streamServerHandle() { return g_server; }

uint8_t streamViewers() { return g_viewers.load(); }

}  // namespace net
}  // namespace cq
