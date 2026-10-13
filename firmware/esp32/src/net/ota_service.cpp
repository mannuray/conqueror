#include "net/ota_service.h"

#include <Arduino.h>
#include <ArduinoOTA.h>

#include "board_config.h"
#include "net/link_task.h"
#include "net/log.h"
#include "secrets.h"

namespace cq {
namespace net {

OtaService::Hook OtaService::s_hooks[OtaService::kMaxHooks] = {};
int OtaService::s_hook_count = 0;

void OtaService::begin() {
  ArduinoOTA.setPort(board::kOtaPort);
  ArduinoOTA.setHostname(board::kHostname);
  ArduinoOTA.setPassword(CQ_OTA_PASSWORD);  // MD5 challenge: "fun, not ultra secure"
  ArduinoOTA.setMdnsEnabled(false);         // WifiManager announces _arduino._tcp
  ArduinoOTA.onStart(onStart);
  ArduinoOTA.onEnd([]() { logf("ota: done - rebooting into the new slot"); });
  ArduinoOTA.onError([](ota_error_t e) {
    logf("ota: error %u - rebooting into the current firmware", static_cast<unsigned>(e));
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();  // clients were closed and heartbeats stopped: start clean
  });
}

void OtaService::addStartHook(Hook hook) {
  if (s_hook_count < kMaxHooks) s_hooks[s_hook_count++] = hook;
}

void OtaService::onStart() {
  logf("ota: start (%s) - stopping the tank and closing clients",
       ArduinoOTA.getCommand() == U_FLASH ? "app" : "source partition");
  LinkCommand cmd{};
  cmd.kind = LinkCommand::Kind::kOtaStart;
  linkPost(cmd, true);  // STOP, close WS/TCP, stop heartbeats
  for (int i = 0; i < s_hook_count; ++i) s_hooks[i]();
  vTaskDelay(pdMS_TO_TICKS(50));  // let link_task send the STOP before flash writes start
}

void OtaService::poll(bool allowed) {
  if (!allowed) return;  // espota times out; retry when nobody drives
  if (!m_started) {
    ArduinoOTA.begin();  // needs the network up: first call after WiFi is ready
    m_started = true;
    logf("ota: ready on port %u (password from secrets.h)", board::kOtaPort);
  }
  ArduinoOTA.handle();
}

}  // namespace net
}  // namespace cq
