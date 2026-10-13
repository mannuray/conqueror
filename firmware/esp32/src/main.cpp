// Conqueror ESP32-S3 developer bridge - composition root (bridge LLD R2.6).
// DEVELOPER BRIDGE - NO AUTHENTICATION - home network only. Never in release firmware.
#include <Arduino.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>

#include "board_config.h"
#include "net/camera_service.h"
#include "net/http_server.h"
#include "net/info_service.h"
#include "net/link_task.h"
#include "net/log.h"
#include "net/ota_service.h"
#include "net/source_archive.h"
#include "net/stream_server.h"
#include "net/web_routes.h"
#include "net/wifi_manager.h"
#include "secrets_guard.h"
#include "version.h"

namespace {

cq::net::WifiManager g_wifi;
cq::net::OtaService g_ota;
uint32_t g_last_status_ms = 0;

void logBanner() {
  using cq::net::logf;
  logf("%s %s commit %.8s%s built %s", CQ_FW_NAME, CQ_FW_VERSION, CQ_GIT_COMMIT,
       CQ_GIT_DIRTY ? " (dirty)" : "", CQ_BUILD_DATE);
  logf("repo %s", CQ_REPO_URL);
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  logf("chip %s rev %u, %u MHz, flash %lu MB, PSRAM %lu MB, MAC %02x:%02x:%02x:%02x:%02x:%02x",
       ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()),
       static_cast<unsigned>(ESP.getCpuFreqMHz()),
       static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
       static_cast<unsigned long>(ESP.getPsramSize() >> 20), mac[0], mac[1], mac[2], mac[3], mac[4],
       mac[5]);
  const esp_partition_t* running = esp_ota_get_running_partition();
  logf("running from partition %s", running ? running->label : "?");
  logf("DEVELOPER BRIDGE - NO AUTHENTICATION - home network only");
}

const char* stateName(cq::core::LinkState s) {
  switch (s) {
    case cq::core::LinkState::kWebHandshake:
      return "web-handshake";
    case cq::core::LinkState::kWebReady:
      return "web-ready";
    case cq::core::LinkState::kWebDriving:
      return "web-driving";
    case cq::core::LinkState::kTcpPassthrough:
      return "bench";
    case cq::core::LinkState::kIdle:
    default:
      return "idle";
  }
}

bool hasClients(const cq::net::LinkStatus& s) {
  return s.ws_clients > 0 || s.tcp_active || cq::net::streamViewers() > 0;
}

void logStatus(uint32_t now_ms, const cq::net::LinkStatus& s) {
  cq::net::logf("status: up %lus wifi %s link %s uno %s ws %u tcp %u rx %lu tx %lu crc %lu heap %lu",
                static_cast<unsigned long>(now_ms / 1000), g_wifi.modeName(), stateName(s.state),
                s.synced ? "synced" : (s.hello.seen ? "seen" : "-"), s.ws_clients,
                s.tcp_active ? 1u : 0u, static_cast<unsigned long>(s.frames_rx),
                static_cast<unsigned long>(s.frames_tx), static_cast<unsigned long>(s.bad_crc),
                static_cast<unsigned long>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  cq::net::logBegin();
  logBanner();
  if (ESP.getPsramSize() == 0) {
    cq::net::logf("ERROR: no PSRAM - check board_build.arduino.memory_type = qio_opi");
  }
  g_wifi.begin(millis());
  cq::net::httpServerStart();
  cq::net::webRoutesRegister();
  cq::net::sourceBegin();
  cq::net::sourceRegister();
  cq::net::infoRegister(g_wifi);
  const bool camera = cq::net::cameraBegin();
  cq::net::linkTaskStart(cq::net::cameraSensorName());
  if (camera) cq::net::streamServerStart();
  g_ota.addStartHook(cq::net::cameraStop);
  g_ota.begin();
}

void loop() {
  const uint32_t now = millis();
  const cq::net::LinkStatus s = cq::net::linkStatus();
  g_wifi.step(now, hasClients(s));
  g_ota.poll(s.ota_allowed && g_wifi.mode() != cq::core::WifiMode::kConnecting);
  if (now - g_last_status_ms >= cq::board::kStatusPeriodMs) {
    g_last_status_ms = now;
    logStatus(now, s);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
