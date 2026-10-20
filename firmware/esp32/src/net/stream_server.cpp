#include "net/stream_server.h"

#include <Arduino.h>
#include <esp_http_server.h>
#include <stdio.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_config.h"
#include "core/stream_header.h"
#include "net/camera_service.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

constexpr const char* kBoundary = "cqframe";
// One FreeRTOS worker per viewer slot (R3.1), created once at start-up and reused (no task create / delete per
// connection): stack, priority, core (core 0 with the HTTP server; the link task is on core 1).
constexpr uint32_t kViewerStack = 6144;
constexpr UBaseType_t kViewerPrio = 5;
constexpr BaseType_t kViewerCore = 0;
std::atomic<uint8_t> g_viewers{0};
QueueHandle_t g_jobs = nullptr;  // async request copies waiting for a free worker

// The frame's capture time in µs: esp32-camera stamps it at VSYNC with esp_timer_get_time().
uint64_t frameUs(const camera_fb_t* fb) {
  return static_cast<uint64_t>(fb->timestamp.tv_sec) * 1000000ULL + static_cast<uint64_t>(fb->timestamp.tv_usec);
}

void releaseViewer() {
  const uint8_t left = static_cast<uint8_t>(--g_viewers);
  logf("stream: viewer left (%u)", static_cast<unsigned>(left));
}

// R3.1: the frame loop runs in a worker task (one per viewer slot) so the server task stays free
// for /capture and the second viewer. Owns the async request copy and the viewer slot while it streams.
void serveStream(httpd_req_t* req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=cqframe");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char part[160];
  esp_err_t err = ESP_OK;
  uint32_t last_ms = 0, seq = 0;
  while (err == ESP_OK) {
    const uint8_t cap = cameraSettings().fps_cap;  // Plan O: frame-rate limit per viewer
    if (cap > 0) {
      const uint32_t gap = 1000u / cap, since = millis() - last_ms;
      if (since < gap) vTaskDelay(pdMS_TO_TICKS(gap - since));
    }
    last_ms = millis();
    camera_fb_t* fb = cameraGrab();
    if (fb == nullptr) break;  // camera stopped (OTA) or failed
    // Plan Q1 (AI engine LLD §2): the capture time (VSYNC, ESP32 timer - the clock of the telemetry's bus_us)
    const size_t len = core::mjpegPartHeader(part, sizeof(part), kBoundary, fb->len, frameUs(fb), ++seq);
    err = httpd_resp_send_chunk(req, part, static_cast<ssize_t>(len));
    if (err == ESP_OK) {
      err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf),
                                  static_cast<ssize_t>(fb->len));
    }
    if (err == ESP_OK) cameraCountFrame(static_cast<uint32_t>(fb->len));
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
  // Known tiny window (final review): the server could close this fd and give the number to a new client before the
  // queued close runs. Accepted - the reverse order lost the close on the tank and blocked port 81.
  httpd_sess_trigger_close(server, sockfd);
  releaseViewer();
}

void streamWorker(void*) {
  for (;;) {
    httpd_req_t* req = nullptr;
    if (xQueueReceive(g_jobs, &req, portMAX_DELAY) == pdTRUE && req != nullptr) serveStream(req);
  }
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
  if (xQueueSend(g_jobs, &copy, 0) != pdTRUE) {  // cannot happen while viewers <= workers; answered anyway
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
  char cap_us[21];  // Plan Q1: the capture time, as on the stream's parts
  core::u64Str(cap_us, sizeof(cap_us), frameUs(fb));
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "X-Frame-Us", cap_us);
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
  g_jobs = xQueueCreate(board::kMaxStreamViewers, sizeof(httpd_req_t*));
  if (g_jobs == nullptr) {
    logf("stream: start failed (queue)");
    return false;
  }
  for (uint8_t i = 0; i < board::kMaxStreamViewers; ++i) {
    if (xTaskCreatePinnedToCore(streamWorker, "cq-stream", kViewerStack, nullptr, kViewerPrio, nullptr, kViewerCore) !=
        pdPASS) {
      logf("stream: start failed (worker %u)", static_cast<unsigned>(i));
      return false;
    }
  }
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
