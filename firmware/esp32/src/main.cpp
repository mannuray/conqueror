// Conqueror ESP32-S3 developer bridge - composition root (bridge LLD R2.6).
// DEVELOPER BRIDGE - NO AUTHENTICATION - home network only. Never in release firmware.
#include <Arduino.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "board_config.h"
#include "net/boot_diag.h"
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
bool g_camera_started = false;

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
    case cq::core::LinkState::kIdle:
    default:
      return "idle";
  }
}

bool hasClients(const cq::net::LinkStatus& s) {
  return s.ws_clients > 0 || cq::net::streamViewers() > 0;
}

void logStatus(uint32_t now_ms, const cq::net::LinkStatus& s) {
  cq::net::logf("status: up %lus wifi %s link %s uno %s ws %u rx %lu tx %lu crc %lu heap %lu",
                static_cast<unsigned long>(now_ms / 1000), g_wifi.modeName(), stateName(s.state),
                s.synced ? "synced" : (s.hello.seen ? "seen" : "-"), s.ws_clients,
                static_cast<unsigned long>(s.frames_rx),
                static_cast<unsigned long>(s.frames_tx), static_cast<unsigned long>(s.bad_crc),
                static_cast<unsigned long>(ESP.getFreeHeap()));
}

}  // namespace

// R15.8 (design-notes §98, §99): the link task and this loop are watched (3 s), and core 0's idle task as before (owner:
// keep that check - a frozen core 0 with WiFi / camera restarts the bridge); camera / stream tasks are not subscribed.
void watchdogBegin() {
  const esp_task_wdt_config_t wdt{3000, 1u << 0, true};  // timeout_ms, idle-task check on core 0, panic -> restart
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);  // the Arduino loop task (setup runs on it)
}

// Plan P Task 13 tank check only (build flag CQ_WDT_TEST): the loop stops feeding the watchdog once, 60 s after boot.
void watchdogTest(uint32_t now) {
#ifdef CQ_WDT_TEST
  static bool s_tested = false;
  if (s_tested || now < 60000) return;
  s_tested = true;
  cq::net::logf("wdt test: loop blocked for 4 s - the bridge must restart (task watchdog)");
  vTaskDelay(pdMS_TO_TICKS(4000));
#else
  (void)now;
#endif
}

void watchdogLoopOff() { esp_task_wdt_delete(nullptr); }  // OTA: the upload blocks loop() for ~30 s

void setup() {
  cq::net::logBegin();
  logBanner();
  cq::net::bootDiagBegin();  // design-notes §38: boot count + reset reason
  if (ESP.getPsramSize() == 0) {
    cq::net::logf("ERROR: no PSRAM - check board_build.arduino.memory_type = qio_opi");
  }
  g_wifi.begin(millis());
  cq::net::httpServerStart();
  cq::net::webRoutesRegister();
  cq::net::sourceBegin();
  cq::net::sourceRedirectRegister();  // port 80 -> 81 (R3.11)
  cq::net::infoRegister(g_wifi);
  watchdogBegin();
  cq::net::linkTaskStart(cq::net::cameraSensorName);
  // The camera starts in loop() once WiFi is up (design-notes §38: no simultaneous current peaks).
  g_ota.addStartHook(cq::net::cameraStop);
  g_ota.addStartHook(watchdogLoopOff);
  g_ota.begin();
}

void loop() {
  esp_task_wdt_reset();
  const uint32_t now = millis();
  watchdogTest(now);
  const cq::net::LinkStatus s = cq::net::linkStatus();
  g_wifi.step(now, hasClients(s));
  cq::net::cameraApplyPending();  // Plan O: camera settings from the page (sensor setters + NVS, off the link task)
  // Staged start (design-notes §38): camera after the station is up, or after 10 s in any case.
  if (!g_camera_started && (g_wifi.mode() == cq::core::WifiMode::kStation || now >= 10000)) {
    g_camera_started = true;
    if (!cq::board::kCameraEnabled) {
      cq::net::logf("camera: disabled (board::kCameraEnabled, LLD R4.14)");
    } else {
      cq::net::cameraBegin();
    }
    // A failed or disabled camera still gets port 81: /source.tar.gz, and /stream /capture answer 503.
    if (cq::net::streamServerStart()) cq::net::sourceRegister(cq::net::streamServerHandle());
  }
  g_ota.poll(s.ota_allowed && g_wifi.mode() != cq::core::WifiMode::kConnecting);
  if (now - g_last_status_ms >= cq::board::kStatusPeriodMs) {
    g_last_status_ms = now;
    logStatus(now, s);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
