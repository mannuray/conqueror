#include "net/info_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "core/ws_json.h"
#include "net/boot_diag.h"
#include "net/camera_service.h"
#include "net/http_server.h"
#include "net/link_task.h"
#include "net/source_archive.h"
#include "net/stream_server.h"
#include "version.h"

namespace cq {
namespace net {
namespace {

const WifiManager* g_wifi = nullptr;
char g_json[2048];  // httpd task only

class Out {
 public:
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(g_json + m_n, sizeof(g_json) - m_n, fmt, ap);
    va_end(ap);
    if (n > 0) m_n = (m_n + static_cast<size_t>(n) < sizeof(g_json)) ? m_n + n : sizeof(g_json) - 1;
  }
  size_t len() const { return m_n; }

 private:
  size_t m_n = 0;
};

const char* resetName(uint8_t r) {
  static const char* const kNames[] = {"unknown", "power_on", "external", "brown_out", "watchdog"};
  return r < 5 ? kNames[r] : "unknown";
}

const char* ownerName(core::LinkState s) {
  switch (s) {
    case core::LinkState::kTcpPassthrough:
      return "bench";
    case core::LinkState::kWebHandshake:
    case core::LinkState::kWebReady:
    case core::LinkState::kWebDriving:
      return "web";
    case core::LinkState::kIdle:
    default:
      return "none";
  }
}

esp_err_t serveInfo(httpd_req_t* req) {
  const LinkStatus s = linkStatus();
  const SourceHeader& src = sourceHeader();
  const bool src_ok = sourceValid();
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ip[16] = "";
  g_wifi->ip(ip, sizeof(ip));
  const bool station = g_wifi->mode() == core::WifiMode::kStation;
  const unsigned rev = ESP.getChipRevision();

  Out o;
  o.add("{\"fw\":{\"name\":\"%s\",\"version\":\"%s\",\"commit\":\"%s\",\"dirty\":%s,"
        "\"built\":\"%s\",\"repo\":\"%s\"},",
        CQ_FW_NAME, CQ_FW_VERSION, CQ_GIT_COMMIT, CQ_GIT_DIRTY ? "true" : "false", CQ_BUILD_DATE,
        CQ_REPO_URL);
  if (src_ok) {
    o.add("\"source\":{\"url\":\"/source.tar.gz\",\"commit\":\"%.40s\",\"matches_fw\":%s,"
          "\"bytes\":%lu},",
          src.commit, strncmp(src.commit, CQ_GIT_COMMIT, 40) == 0 ? "true" : "false",
          static_cast<unsigned long>(src.length));
  } else {
    o.add("\"source\":{\"url\":null,\"commit\":null,\"matches_fw\":false,\"bytes\":0},");
  }
  o.add("\"hw\":{\"board\":\"%s\",\"chip\":\"%s rev v%u.%u\",\"flash_mb\":%lu,\"psram_mb\":%lu,"
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"camera\":\"%s\","
        "\"pins\":{\"uno_rx\":%d,\"uno_tx\":%d,\"camera\":\"ESP32-S3-EYE map\"}},",
        board::kBoardName, ESP.getChipModel(), rev / 100, rev % 100,
        static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
        static_cast<unsigned long>(ESP.getPsramSize() >> 20), mac[0], mac[1], mac[2], mac[3],
        mac[4], mac[5], cameraSensorName(), board::kUnoRxPin, board::kUnoTxPin);
  char ssid[72];  // review M10: escaped, so a " or \\ in the SSID keeps /info valid JSON
  core::jsonEscape(ssid, sizeof(ssid), station ? WiFi.SSID().c_str() : board::kFallbackSsid);
  o.add("\"net\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
        "\"mdns\":\"%s.local\",\"ports\":{\"http\":%u,\"ws\":\"/ws\",\"stream\":%u,"
        "\"bench_tcp\":%u,\"ota\":%u}},",
        g_wifi->modeName(), ssid, ip,
        g_wifi->rssi(), board::kHostname, board::kHttpPort, board::kStreamPort, board::kTcpPort,
        board::kOtaPort);
  if (s.hello.seen) {
    char commit[12] = "null";
    if (s.hello.commit != 0) {
      snprintf(commit, sizeof(commit), "\"%08lx\"", static_cast<unsigned long>(s.hello.commit));
    }
    o.add("\"uno\":{\"baud\":%lu,\"synced\":%s,\"fw\":\"%u.%u\",\"commit\":%s,\"proto\":%u,"
          "\"reset_reason\":\"%s\"},",
          static_cast<unsigned long>(board::kUnoBaud), s.synced ? "true" : "false",
          s.hello.fw >> 8, s.hello.fw & 0xFF, commit, s.hello.proto,
          resetName(s.hello.reset_reason));
  } else {
    o.add("\"uno\":{\"baud\":%lu,\"synced\":null,\"fw\":null,\"commit\":null,\"proto\":null,"
          "\"reset_reason\":null},",
          static_cast<unsigned long>(board::kUnoBaud));
  }
  o.add("\"link\":{\"owner\":\"%s\",\"driver\":\"%s\",\"clients\":{\"ws\":%u,\"tcp\":%u,"
        "\"stream\":%u},\"frames_tx\":%lu,\"frames_rx\":%lu,\"bad_crc\":%lu,\"tx_dropped\":%lu,"
        "\"tcp_refused\":%lu,\"last_refusal\":\"%s\",\"ota_allowed\":%s},",
        ownerName(s.state), s.driver, s.ws_clients, s.tcp_active ? 1u : 0u, streamViewers(),
        static_cast<unsigned long>(s.frames_tx), static_cast<unsigned long>(s.frames_rx),
        static_cast<unsigned long>(s.bad_crc), static_cast<unsigned long>(s.tx_dropped),
        static_cast<unsigned long>(s.tcp_refused),
        s.last_refusal, s.ota_allowed ? "true" : "false");
  char boot[200];
  if (bootDiagJson(boot, sizeof(boot)) == 0) snprintf(boot, sizeof(boot), "null");
  o.add("\"boot\":%s,", boot);  // design-notes §38
  o.add("\"uptime_s\":%lu,\"free_heap\":%lu,\"free_psram\":%lu}",
        static_cast<unsigned long>(millis() / 1000), static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getFreePsram()));

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, g_json, static_cast<ssize_t>(o.len()));
}

}  // namespace

void infoRegister(const WifiManager& wifi) {
  g_wifi = &wifi;
  httpd_uri_t u{};
  u.uri = "/info";
  u.method = HTTP_GET;
  u.handler = serveInfo;
  httpd_register_uri_handler(httpServer(), &u);
}

}  // namespace net
}  // namespace cq
