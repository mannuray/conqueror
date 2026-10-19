#include "core/motion_config.h"
#include "core/acked_ops.h"
#include "core/arc_limits.h"
#include "core/confirm.h"
#include "core/move_controller.h"
#include "core/roll_learner.h"
#include "net/motion_store.h"
#include "net/wifi_manager.h"
#include "core/event_log.h"
#include "app/replies.h"
#include "app/motion_controller.h"
#include "app/settings_controller.h"
#include "app/telemetry_publisher.h"
#include "app/uno_channel.h"
#include "app/uno_event_router.h"
#include "app/tank_app_controller.h"
#include "net/camera_service.h"
#include "net/stream_server.h"
#include <esp_system.h>
#include "net/link_task.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "core/frame_tx.h"
#include "core/ws_clients.h"
#include "core/ws_codec.h"
#include "core/motion_router.h"
#include "core/motion_link.h"
#include "core/ws_json.h"
#include "net/http_server.h"
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

UartPort g_uart;
core::FrameTx g_tx;
core::BodyLink g_link(g_tx);
core::LinkOwner g_owner;
core::WsClients g_ws;
bool g_ota_active = false;

// WS bookkeeping (link_task only)
char g_json[core::kWsJsonMax];
core::EventLog g_log;          // Plan O (LLD R13): the Log tab
app::Replies g_out(g_ws);      // Plan P: the controllers' replies (LLD R15.3)
app::TankAppController g_tank(g_out, g_log, g_link);  // Plan P Task 6
app::SettingsController g_settings(g_out, g_log, g_link);  // Plan P Task 7
app::TelemetryPublisher g_tel(g_out, g_link);  // Plan P Task 8
app::UnoEventRouter g_events(g_out, g_log, g_tel, g_link);  // Plan P Task 8
app::MotionController g_motionc(g_out, g_log, g_link, g_owner, g_tel);  // Plan P Task 9
app::UnoChannel g_uno(g_link, g_tx, g_out);  // Plan P Task 10: every acked Uno request
uint32_t g_last_slow_count = 0;
// Plan M (bridge LLD R9): confirmations by the tank's button, the WiFi double press, acked Uno ops for the page.
core::StopBarrier g_stop_barrier;  // STOP fast path: drop motion posted before a STOP

void drainTx() {
  const uint8_t* p = nullptr;
  size_t len = 0;
  while ((len = g_tx.peek(p)) != 0) {
    if (g_uart.txFree() < len) return;  // next tick (UART drains 25 B/ms); never a partial chunk
    g_uart.write(p, len);
    g_tx.pop();
  }
}

core::OwnerView ownerView() {
  if (g_owner.irActive()) return {"ir", ""};
  if (g_owner.driverFd() != core::LinkOwner::kNoFd) return {"human", g_ws.name(g_owner.driverFd())};
  return {"none", ""};
}

bool bodyOffline() { return !g_link.synced(); }

uint32_t tankMs() { return g_link.snapshot().fast_valid ? g_link.snapshot().fast.uno_ms : 0; }

void sendTo(int fd, size_t len) {
  if (len) wsSend(fd, g_json, len);
}

void broadcast(size_t len) {
  if (len == 0) return;
  for (size_t i = 0; i < g_ws.count(); ++i) wsSend(g_ws.fdAt(i), g_json, len);
}

void nack(int fd, int32_t id, core::BridgeReason r);

void apply(const core::Actions& a, uint32_t now) {
  if (a.has(core::Actions::kSendStop)) g_link.sendStop(now);
  if (a.has(core::Actions::kHintNone)) g_link.setOwnerHint(proto::OwnerHintValue::kNone);
  if (a.has(core::Actions::kHintHuman)) g_link.setOwnerHint(proto::OwnerHintValue::kHuman);
  if (a.has(core::Actions::kStartMaster)) g_link.startMaster(now);
  if (a.has(core::Actions::kOwnerChanged)) {
    broadcast(core::jsonControlChanged(g_json, sizeof(g_json), g_tel.nextEventSeq(), tankMs(), ownerView()));
    g_tel.sendTs();
  }
}

void nack(int fd, int32_t id, core::BridgeReason r) {
  sendTo(fd, core::jsonNack(g_json, sizeof(g_json), id, r));
}

core::BridgeReason reasonOf(core::Decision d) {
  switch (d) {
    case core::Decision::kNackBusy:
      return core::BridgeReason::kBusy;
    case core::Decision::kNackBodyOffline:
      return core::BridgeReason::kBodyOffline;
    case core::Decision::kNackStopped:
      return core::BridgeReason::kStopped;
    case core::Decision::kNackNotOwner:
    default:
      return core::BridgeReason::kNotOwner;
  }
}

// drive / cam_joy are streaming: a rejection is reported once (nack without id), not per message.
void streamReject(int fd, core::BridgeReason r) {
  if (g_ws.reportStreamReject(fd, static_cast<uint8_t>(r))) nack(fd, -1, r);
}



// Streaming commands (one rejection reported per stream) and every motion command (dropped behind a STOP).
bool isStream(core::WsCmdKind k) {
  return k == core::WsCmdKind::kDrive || k == core::WsCmdKind::kCamJoy || k == core::WsCmdKind::kDriveRate;
}
bool isMotion(core::WsCmdKind k) {
  return isStream(k) || k == core::WsCmdKind::kTurn || k == core::WsCmdKind::kMove;
}

// ---------- Plan M (spec 02 Part I, bridge LLD R9): WiFi, settings page, confirmations ----------

// STOP fast path: may overtake its own kWsOpen - never lost.
void handleStop(const LinkCommand& cmd, int32_t id, uint32_t now) {
  const int fd = cmd.fd;
  g_stop_barrier.onStop(cmd.post_seq);
  if (g_owner.state() == core::LinkState::kWebDriving && g_owner.driverFd() != fd) {  // Plan O: §72 stop hold
    g_log.add(core::LogKind::kStopHold, 0, 0, g_ws.contains(fd) ? g_ws.name(fd) : "?", tankMs(), now);
  }
  apply(g_owner.onStop(fd), now);  // urgent slot; queued DRIVE/CAM_JOY frames are purged
  g_motionc.end(core::MotionEvent::kStop, now);  // Plan H: a STOP from anyone ends a primitive
  if (!g_ws.contains(fd)) return;
  g_ws.admit(fd, core::WsCmdKind::kStop, now);  // never limited; counts as alive
  g_uno.waitStop(fd, id);  // answered when the Uno acks the STOP
}

// False when the command is dropped (stale behind a STOP) or rate limited (answered).
bool admitted(const LinkCommand& cmd, core::WsCmdKind kind, int32_t id, uint32_t now) {
  const int fd = cmd.fd;
  if (isMotion(kind) && g_stop_barrier.stale(cmd.post_seq)) {
    return false;  // posted before a STOP that was handled first: silently dropped
  }
  if (g_ws.admit(fd, kind, now)) return true;
  if (isStream(kind)) {
    streamReject(fd, core::BridgeReason::kRateLimited);
  } else {
    nack(fd, id, core::BridgeReason::kRateLimited);
  }
  return false;
}

void handleDrive(int fd, const core::WsCommand& c, uint32_t now) {
  if (g_ota_active) {
    streamReject(fd, core::BridgeReason::kBusy);
    return;
  }
  core::Actions a;
  const core::Decision d = g_owner.onDrive(fd, now, a, c.v != 0 || c.w != 0);  // §72: a held stick stays stopped
  apply(a, now);
  if (d == core::Decision::kSend && g_motionc.ownedBy(fd)) g_motionc.end(core::MotionEvent::kOwnerCommand, now);
  if (d == core::Decision::kSend && (c.v != 0 || c.w != 0)) g_motionc.cancelRollOn();  // §G6: driving again, no roll-on
  if (d != core::Decision::kSend) {
    streamReject(fd, reasonOf(d));
  } else if (g_link.sendDrive(c.v, c.w)) {  // 1:1 - the bridge never repeats a DRIVE
    g_ws.streamAccepted(fd);
  } else {
    streamReject(fd, core::BridgeReason::kBodyOffline);
  }
}

void handleCamJoy(int fd, const core::WsCommand& c) {
  const core::Decision d = g_owner.onCamera(fd);
  if (d != core::Decision::kSend || g_ota_active) {
    streamReject(fd, g_ota_active ? core::BridgeReason::kBusy : reasonOf(d));
  } else if (g_link.sendCamJoy(c.x, c.y, c.active)) {
    g_ws.streamAccepted(fd);
  }
}

void handleCamHome(int fd, int32_t id) {
  const core::Decision d = g_owner.onCamera(fd);
  if (d != core::Decision::kSend) return nack(fd, id, app::reasonOf(d));
  core::AckedOp op;
  op.kind = core::AckedOp::kCamHome;
  op.fd = fd;
  op.id = id;
  if (!g_uno.request(op)) nack(fd, id, core::BridgeReason::kBusy);  // R15.5: waits in the queue; BUSY only when full
}

// SOUND / AUDIO_MODE (R3.1) / CAM_LOCK (Plan F): owner or nobody (as the camera); acked by the Uno, one at a time.
void handleAcked(int fd, int32_t id, const core::WsCommand& c) {
  const core::Decision d = g_owner.onCamera(fd);
  if (d != core::Decision::kSend || g_ota_active) {
    return nack(fd, id, g_ota_active ? core::BridgeReason::kBusy : app::reasonOf(d));
  }
  core::AckedOp op;
  op.kind = c.kind == core::WsCmdKind::kSound       ? core::AckedOp::kSound
            : c.kind == core::WsCmdKind::kAudioMode ? core::AckedOp::kAudioMode
                                                    : core::AckedOp::kCamLock;
  op.a = c.kind == core::WsCmdKind::kSound ? c.sound_id : (c.kind == core::WsCmdKind::kAudioMode ? c.audio_mode : c.on);
  op.fd = fd;
  op.id = id;
  if (!g_uno.request(op)) nack(fd, id, core::BridgeReason::kBusy);  // R15.5: waits in the queue; BUSY only when full
}

// LLD R5: opt-in debug stream for this client only (any role: read-only data).
void handleDebug(int fd, int32_t id, const core::WsCommand& c) {
  g_ws.setDebug(fd, c.on ? static_cast<uint8_t>(c.debug_hz == 50 ? 1 : 5) : 0);
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

void handleRelease(int fd, int32_t id, uint32_t now) {
  apply(g_owner.onRelease(fd), now);
  if (g_motionc.ownedBy(fd)) g_motionc.end(core::MotionEvent::kRelease, now);  // review I1: release ends a primitive
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

void handleWsText(const LinkCommand& cmd, uint32_t now) {
  const int fd = cmd.fd;
  const core::WsCommand c = core::parseWsCommand(cmd.text, cmd.len);
  const int32_t id = c.has_id ? c.id : -1;
  if (c.kind == core::WsCmdKind::kStop) {
    handleStop(cmd, id, now);
    return;
  }
  if (!g_ws.contains(fd) || !admitted(cmd, c.kind, id, now)) return;
  switch (c.kind) {
    case core::WsCmdKind::kHello: g_tel.hello(fd, CQ_FW_VERSION, g_sensor()); break;
    case core::WsCmdKind::kDrive: handleDrive(fd, c, now); break;
    case core::WsCmdKind::kStop: break;  // handled above (fast path)
    case core::WsCmdKind::kCamJoy: handleCamJoy(fd, c); break;
    case core::WsCmdKind::kCamHome: handleCamHome(fd, id); break;
    case core::WsCmdKind::kSound:
    case core::WsCmdKind::kAudioMode:
    case core::WsCmdKind::kCamLock: handleAcked(fd, id, c); break;
    case core::WsCmdKind::kTurn: g_motionc.onTurn(fd, id, c, now); break;
    case core::WsCmdKind::kDriveRate: g_motionc.onDriveRate(fd, c, now); break;
    case core::WsCmdKind::kCancel: g_motionc.onCancel(fd, id, now); break;
    case core::WsCmdKind::kMove: g_motionc.onMove(fd, id, c, now); break;
    case core::WsCmdKind::kCalib: g_settings.onCalib(fd, id, c, now); break;
    case core::WsCmdKind::kWifiStatus:
    case core::WsCmdKind::kWifiScan:
    case core::WsCmdKind::kWifiSave:
    case core::WsCmdKind::kWifiForget:
    case core::WsCmdKind::kWifiMode:
    case core::WsCmdKind::kApPassword:
    case core::WsCmdKind::kSetting:
    case core::WsCmdKind::kRestart: g_settings.onCommand(fd, id, c, now); break;
    case core::WsCmdKind::kRelease: handleRelease(fd, id, now); break;
    case core::WsCmdKind::kTank: g_tank.onTank(fd, now); break;
    case core::WsCmdKind::kLog: g_tank.onLog(fd, c.log_since, now); break;
    case core::WsCmdKind::kCameraSet: g_tank.onCameraSet(fd, id, c, now); break;
    case core::WsCmdKind::kSelfTest: g_tank.onSelfTest(fd, id); break;
    case core::WsCmdKind::kAppSet: g_tank.onAppSet(fd, id, c); break;
    case core::WsCmdKind::kDebug: handleDebug(fd, id, c); break;
    case core::WsCmdKind::kPing: sendTo(fd, core::jsonPong(g_json, sizeof(g_json), c.t0, now)); break;
    case core::WsCmdKind::kUnsupported:
    case core::WsCmdKind::kInvalid:
    default: nack(fd, id, core::BridgeReason::kInvalidCommand); break;
  }
}

void closeAllClients() {
  for (size_t i = 0; i < g_ws.count(); ++i) wsClose(g_ws.fdAt(i));  // kWsClose follows
}

void handleCommand(const LinkCommand& cmd, uint32_t now) {
  switch (cmd.kind) {
    case LinkCommand::Kind::kWsOpen:
      if (g_ota_active || !g_ws.add(cmd.fd, now)) {
        wsClose(cmd.fd);  // full (4 clients) or updating
        return;
      }
      logf("ws: %s connected (%u clients)", g_ws.name(cmd.fd), static_cast<unsigned>(g_ws.count()));
      apply(g_owner.onWsOpen(cmd.fd), now);
      break;
    case LinkCommand::Kind::kWsClose: {
      if (!g_ws.contains(cmd.fd)) return;  // a plain HTTP socket
      char name[12];
      snprintf(name, sizeof(name), "%s", g_ws.name(cmd.fd));
      if (g_motionc.ownedBy(cmd.fd)) g_motionc.end(core::MotionEvent::kOwnerClosed, now);  // onWsClose STOPs its driver
      if (cmd.fd == g_owner.driverFd()) g_log.add(core::LogKind::kClientLost, 0, 0, name, tankMs(), now);  // Plan O
      const core::Actions a = g_owner.onWsClose(cmd.fd);
      g_settings.onClientClosed(cmd.fd);  // Plan M: a waiting change of that client is dropped (review focus 4)
      g_ws.remove(cmd.fd);
      logf("ws: %s closed (%u clients)", name, static_cast<unsigned>(g_ws.count()));
      apply(a, now);
      break;
    }
    case LinkCommand::Kind::kWsText:
      handleWsText(cmd, now);
      break;
    case LinkCommand::Kind::kOtaStart:
    default:
      g_ota_active = true;  // from now on: drives nacked BUSY, new phones refused
      g_log.add(core::LogKind::kOta, 0, 0, "", tankMs(), now);
      g_motionc.end(core::MotionEvent::kOta, now);  // the STOP below stops it
      g_link.sendStop(now);  // resent until the Uno's ACK
      g_uno.stopForOta();  // heartbeats stop: Uno link-loss stop at standstill is harmless (design-notes §95)
      closeAllClients();
      break;
  }
}

void handleLinkEvents(uint32_t now) {
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) g_events.route(ev, now);
}

void telemetry(uint32_t now) {
  const app::TelemetryPublisher::News news = g_tel.poll();  // td / tds to the clients that asked
  if (news.fast) {
    g_motionc.onTelemetry(now);  // roll-on + the motion service step (50 Hz)
    g_tel.afterFast();          // tf at 10 Hz
  }
  g_motionc.tick(now);  // telemetry gap / drive_rate deadman
  g_tel.tickTs(now);    // 1 Hz, also while the Uno is silent (body_offline)
}

void reapSilent(uint32_t now) {
  int fds[core::kMaxWsClients];
  const size_t n = g_ws.silent(now, fds, core::kMaxWsClients);
  for (size_t i = 0; i < n; ++i) {
    logf("ws: %s silent for 3 s - closing", g_ws.name(fds[i]));
    if (!wsClose(fds[i])) {  // kWsClose follows -> LinkOwner STOP if it was the driver
      LinkCommand gone{};    // review M6: a ghost (session already gone) - remove it here
      gone.kind = LinkCommand::Kind::kWsClose;
      gone.fd = fds[i];
      handleCommand(gone, now);
    }
  }
}

void publishStatus() {
  LinkStatus s;
  s.state = g_owner.state();
  s.ota_active = g_ota_active;
  s.ota_allowed = g_owner.otaAllowed() && !g_ota_active;
  s.synced = g_link.synced();
  s.hello = g_link.hello();
  snprintf(s.driver, sizeof(s.driver), "%s", ownerView().client);
  s.ws_clients = static_cast<uint8_t>(g_ws.count());
  s.frames_tx = g_link.stats().frames_tx;
  s.frames_rx = g_link.stats().frames_rx;
  s.bad_crc = g_link.parserStats().bad_crc;
  s.tx_dropped = g_tx.dropped();
  portENTER_CRITICAL(&g_status_mux);
  g_status = s;
  portEXIT_CRITICAL(&g_status_mux);
}

void linkTask(void*) {
  uint8_t rx[256];
  uint32_t last_publish = 0;
  TickType_t wake = xTaskGetTickCount();
  apply(g_owner.begin(), millis());  // R14 (design-notes §95): master of the Uno link from boot, with or without phones
  for (;;) {
    const uint32_t now = millis();
    size_t n = 0;
    while ((n = g_uart.read(rx, sizeof(rx))) != 0) {
      g_link.onUartBytes(rx, n, now);  // snapshot, events, STOP ACKs
    }
    LinkCommand cmd;
    while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) handleCommand(cmd, now);
    // Only a NEW slow frame updates the owner (review I1): re-applying the 1 Hz frame's older owner
    // every tick would undo an IR takeover reported by EVENT control_changed.
    if (g_link.snapshot().slow_count != g_last_slow_count) {
      g_last_slow_count = g_link.snapshot().slow_count;
      apply(g_owner.onUnoOwner(static_cast<proto::Owner>(g_link.snapshot().slow.owner)), now);
    }
    apply(g_owner.tick(now), now);
    reapSilent(now);
    g_link.step(now);
    handleLinkEvents(now);
    telemetry(now);
    g_settings.tick(now);  // Plan M: confirmations, WiFi results, restart
    g_uno.tick(now);  // the next acked Uno request
    drainTx();
    if (now - last_publish >= 100) {
      last_publish = now;
      publishStatus();
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

void wireTelemetry() {
  g_tel.setViews({[] { return ownerView(); },
                  [] { return g_motionc.view(); },
                  [] { return bodyOffline(); }});
}

void wireEvents() {
  app::UnoEventRouter::Sinks sinks{};
  sinks.body_online = [](uint32_t now) { apply(g_owner.onSynced(true), now); };
  sinks.body_offline = [](uint8_t, int16_t, uint32_t now) {
    g_motionc.end(core::MotionEvent::kBodyOffline, now);
    apply(g_owner.onSynced(false), now);
    g_uno.onBodyOffline();  // every waiting request answered (Review Focus 2)
  };
  sinks.uno_owner = [](uint8_t o, uint32_t now) { apply(g_owner.onUnoOwner(static_cast<proto::Owner>(o)), now); };
  sinks.button_press = [](uint32_t now) { g_settings.onButtonPress(now); };
  sinks.motion_event = [](uint8_t e, uint32_t now) {
    g_motionc.end(e == static_cast<uint8_t>(proto::EventKind::kStall) ? core::MotionEvent::kBodyStall
                                                                    : core::MotionEvent::kBodyEncoderFault, now);
  };
  sinks.ack_result = [](uint8_t seq, uint8_t status) { g_uno.onAck(seq, status); };
  sinks.stop_result = [](uint8_t status) { g_uno.onStopResult(status); };
  sinks.setting_value = [](uint8_t id, int32_t v) { g_uno.onSettingValue(id, v); };
  g_events.setSinks(sinks);
}

void wireSettings() {
  g_settings.setHooks({[] { return g_motionc.active(); }, [] { return g_ota_active; },
                       [] { return g_owner.driverFd(); }, [](int fd) { g_motionc.sendCalib(fd); },
                       [](const core::AckedOp& op) { return g_uno.request(op); }, &g_motionc.cpm(), &g_motionc.rollMm()});
}

void wireTank() {
  g_tank.setSelfTestHooks({[] { return bodyOffline(); },
                           [] {
                             const core::TankSnapshot& s = g_link.snapshot();
                             return g_motionc.active() || g_owner.state() == core::LinkState::kWebDriving ||
                                    (s.slow_valid && s.slow.driving != 0);  // the tank must stand still
                           },
                           [](int fd, int32_t id) {
                             core::AckedOp op;
                             op.kind = core::AckedOp::kSelfTest;
                             op.fd = fd;
                             op.id = id;
                             return g_uno.request(op);
                           }});
}

void wireMotion() {
  g_motionc.setHooks({[](int fd, uint32_t now) {
                        core::Actions a;
                        const core::Decision d = g_owner.onDrive(fd, now, a);
                        apply(a, now);
                        return d;
                      },
                      [] { return g_ota_active; }});
}

void wireControllers() {
  wireMotion();
  wireTelemetry();
  wireEvents();
  wireSettings();
  wireTank();
}

}  // namespace

void linkTaskStart(const char* (*camera_sensor)()) {
  g_sensor = camera_sensor;
  g_motionc.begin();  // Plan K: counts per metre and roll-on, before the link task runs
  g_tank.begin();  // Plan O / Plan P: the AI address
  wireControllers();  // Plan P: hooks between the controllers (interim until Task 12)
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
  return (to_front ? xQueueSendToFront(g_queue, &cmd, wait)
                   : xQueueSendToBack(g_queue, &cmd, wait)) == pdTRUE;
}

LinkStatus linkStatus() {
  portENTER_CRITICAL(&g_status_mux);
  const LinkStatus s = g_status;
  portEXIT_CRITICAL(&g_status_mux);
  return s;
}

}  // namespace net
}  // namespace cq
