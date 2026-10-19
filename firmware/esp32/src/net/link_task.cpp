// The link task (bridge LLD R2.7, R14, R15): composition root of the controllers and the 2 ms loop on core 1.
#include "net/link_task.h"

#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "app/command_router.h"
#include "app/drive_controller.h"
#include "app/motion_controller.h"
#include "app/replies.h"
#include "app/settings_controller.h"
#include "app/tank_app_controller.h"
#include "app/telemetry_publisher.h"
#include "app/uno_channel.h"
#include "app/uno_event_router.h"
#include "board_config.h"
#include "core/body_link.h"
#include "core/event_log.h"
#include "core/frame_tx.h"
#include "core/link_owner.h"
#include "core/ws_clients.h"
#include "core/ws_codec.h"
#include "core/ws_json.h"
#include "net/log.h"
#include "net/uart_port.h"
#include "version.h"

namespace cq {
namespace net {
namespace {

QueueHandle_t g_queue = nullptr;
portMUX_TYPE g_status_mux = portMUX_INITIALIZER_UNLOCKED;
LinkStatus g_status;
const char* noSensor() { return "none"; }
const char* (*g_sensor)() = noSensor;

// The objects, in dependency order (all static; link task only).
UartPort g_uart;
core::FrameTx g_tx;
core::BodyLink g_link(g_tx);
core::LinkOwner g_owner;
core::WsClients g_ws;
core::EventLog g_log;  // Plan O (LLD R13): the Log tab
app::Replies g_out(g_ws);
app::TelemetryPublisher g_tel(g_out, g_link);
app::UnoChannel g_uno(g_link, g_tx, g_out);
app::MotionController g_motion(g_out, g_log, g_link, g_owner, g_tel);
app::SettingsController g_settings(g_out, g_log, g_link);
app::TankAppController g_tank(g_out, g_log, g_link);
app::DriveController g_drive(g_out, g_log, g_owner, g_uno, g_motion, g_settings, g_tel);
app::UnoEventRouter g_events(g_out, g_log, g_tel, g_link);
app::CommandRouter g_router(g_drive, g_motion, g_settings, g_tank, g_tel, g_out);

void drainTx() {
  const uint8_t* p = nullptr;
  size_t len = 0;
  while ((len = g_tx.peek(p)) != 0) {
    if (g_uart.txFree() < len) return;  // next tick (UART drains 25 B/ms); never a partial chunk
    g_uart.write(p, len);
    g_tx.pop();
  }
}

void handleCommand(const LinkCommand& cmd, uint32_t now) {
  switch (cmd.kind) {
    case LinkCommand::Kind::kWsOpen: g_drive.onClientOpen(cmd.fd, now); break;
    case LinkCommand::Kind::kWsClose: g_drive.onClientClose(cmd.fd, now); break;
    case LinkCommand::Kind::kWsText: g_router.onText(cmd.fd, cmd.text, cmd.len, cmd.post_seq, now); break;
    case LinkCommand::Kind::kOtaStart:
    default: g_drive.startOta(now); break;
  }
}

void telemetry(uint32_t now) {
  const app::TelemetryPublisher::News news = g_tel.poll();  // td / tds to the clients that asked
  if (news.fast) {
    g_motion.onTelemetry(now);  // roll-on + the motion service step (50 Hz)
    g_tel.afterFast();          // tf at 10 Hz
  }
  g_motion.tick(now);  // telemetry gap / drive_rate deadman
  g_tel.tickTs(now);   // 1 Hz, also while the Uno is silent (body_offline)
}

void publishStatus() {
  LinkStatus s;
  s.state = g_owner.state();
  s.ota_active = g_drive.otaActive();
  s.ota_allowed = g_owner.otaAllowed() && !g_drive.otaActive();
  s.synced = g_link.synced();
  s.hello = g_link.hello();
  snprintf(s.driver, sizeof(s.driver), "%s", g_drive.ownerView().client);
  s.ws_clients = static_cast<uint8_t>(g_ws.count());
  s.frames_tx = g_link.stats().frames_tx;
  s.frames_rx = g_link.stats().frames_rx;
  s.bad_crc = g_link.parserStats().bad_crc;
  s.tx_dropped = g_tx.dropped();
  portENTER_CRITICAL(&g_status_mux);
  g_status = s;
  portEXIT_CRITICAL(&g_status_mux);
}

void step(uint32_t now) {
  uint8_t rx[256];
  size_t n = 0;
  while ((n = g_uart.read(rx, sizeof(rx))) != 0) g_link.onUartBytes(rx, n, now);  // snapshot, events, STOP ACKs
  LinkCommand cmd;
  while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) handleCommand(cmd, now);
  g_drive.tick(now);  // owner from a new slow frame, the 5 s handback, silent clients
  g_link.step(now);
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) g_events.route(ev, now);
  telemetry(now);
  g_settings.tick(now);  // confirmations, WiFi results, restart
  g_uno.tick(now);       // the next acked Uno request
  drainTx();
}

void linkTask(void*) {
  uint32_t last_publish = 0;
  TickType_t wake = xTaskGetTickCount();
  g_drive.begin(millis());  // R14 (design-notes §95): master of the Uno link from boot, with or without phones
  esp_task_wdt_add(nullptr);  // R15.8: a stuck link task restarts the bridge after 3 s
  for (;;) {
    const uint32_t now = millis();
    esp_task_wdt_reset();
    step(now);
    if (now - last_publish >= 100) {
      last_publish = now;
      publishStatus();
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

// ---- wiring: the hooks between the controllers (captureless functions; no virtual, standard §1.1) ----
void wireViews() {
  g_tel.setViews({[] { return g_drive.ownerView(); }, [] { return g_motion.view(); }, [] { return !g_link.synced(); }});
  g_motion.setHooks({[](int fd, uint32_t now) { return g_drive.claim(fd, now); }, [] { return g_drive.otaActive(); }});
}

void wireEvents() {
  app::UnoEventRouter::Sinks sinks{};
  sinks.body_online = [](uint32_t now) { g_drive.onSynced(true, now); };
  sinks.body_offline = [](uint8_t, int16_t, uint32_t now) {
    g_motion.end(core::MotionEvent::kBodyOffline, now);
    g_drive.onSynced(false, now);
    g_uno.onBodyOffline();  // every waiting request answered (Review Focus 2)
  };
  sinks.uno_owner = [](uint8_t o, uint32_t now) { g_drive.onUnoOwner(o, now); };
  sinks.button_press = [](uint32_t now) { g_settings.onButtonPress(now); };
  sinks.motion_event = [](uint8_t e, uint32_t now) {
    g_motion.end(e == static_cast<uint8_t>(proto::EventKind::kStall) ? core::MotionEvent::kBodyStall
                                                                     : core::MotionEvent::kBodyEncoderFault, now);
  };
  sinks.ack_result = [](uint8_t seq, uint8_t status) { g_uno.onAck(seq, status); };
  sinks.stop_result = [](uint8_t status) { g_uno.onStopResult(status); };
  sinks.setting_value = [](uint8_t id, int32_t v) { g_uno.onSettingValue(id, v); };
  g_events.setSinks(sinks);
}

bool standingStill() {  // the self-test needs a tank that is not driving
  const core::TankSnapshot& s = g_link.snapshot();
  return !g_motion.active() && g_owner.state() != core::LinkState::kWebDriving && !(s.slow_valid && s.slow.driving != 0);
}

bool queueSelfTest(int fd, int32_t id) {
  core::AckedOp op;
  op.kind = core::AckedOp::kSelfTest;
  op.fd = fd;
  op.id = id;
  return g_uno.request(op);
}

void wireSettingsAndTank() {
  g_settings.setHooks({[] { return g_motion.active(); }, [] { return g_drive.otaActive(); },
                       [] { return g_owner.driverFd(); }, [](int fd) { g_motion.sendCalib(fd); },
                       [](const core::AckedOp& op) { return g_uno.request(op); }, &g_motion.cpm(), &g_motion.rollMm()});
  g_tank.setSelfTestHooks({[] { return !g_link.synced(); }, [] { return !standingStill(); }, queueSelfTest});
}

}  // namespace

void linkTaskStart(const char* (*camera_sensor)()) {
  g_sensor = camera_sensor;
  g_motion.begin();  // Plan K: counts per metre and roll-on, before the link task runs
  g_tank.begin();    // Plan O: the AI address
  g_router.setHello(CQ_FW_VERSION, g_sensor);
  wireViews();
  wireEvents();
  wireSettingsAndTank();
  g_log.add(core::LogKind::kBoot, static_cast<int32_t>(esp_reset_reason()), 0, "", 0, millis());
  g_uart.begin();
  g_queue = xQueueCreate(16, sizeof(LinkCommand));
  xTaskCreatePinnedToCore(linkTask, "link", 8192, nullptr, 5, nullptr, 1);
  logf("uno link: Serial1 RX %d TX %d %lu baud", board::kUnoRxPin, board::kUnoTxPin,
       static_cast<unsigned long>(board::kUnoBaud));
}

bool linkPost(const LinkCommand& cmd, bool to_front) {
  if (g_queue == nullptr) return false;
  const TickType_t wait = pdMS_TO_TICKS(100);  // drained every 2 ms: waits only if 16 are queued
  return (to_front ? xQueueSendToFront(g_queue, &cmd, wait) : xQueueSendToBack(g_queue, &cmd, wait)) == pdTRUE;
}

LinkStatus linkStatus() {
  portENTER_CRITICAL(&g_status_mux);
  const LinkStatus s = g_status;
  portEXIT_CRITICAL(&g_status_mux);
  return s;
}

}  // namespace net
}  // namespace cq
