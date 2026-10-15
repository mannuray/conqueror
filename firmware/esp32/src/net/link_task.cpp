#include "net/link_task.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "core/frame_tx.h"
#include "core/ws_clients.h"
#include "core/ws_codec.h"
#include "core/ws_json.h"
#include "net/http_server.h"
#include "net/log.h"
#include "net/tcp_passthrough.h"
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
TcpPassthrough g_tcp;
core::WsClients g_ws;
bool g_ota_active = false;

// WS bookkeeping (link_task only)
char g_json[core::kWsJsonMax];
uint32_t g_ev_seq = 0;
uint32_t g_tf_seq = 0;
uint32_t g_last_fast_count = 0;
uint32_t g_last_slow_count = 0;
uint32_t g_last_ts_ms = 0;
struct Waiter {
  int fd;
  int32_t id;
};
constexpr size_t kMaxStopWaiters = 2 * core::kMaxWsClients;  // review M8
Waiter g_stop_waiters[kMaxStopWaiters];
size_t g_stop_waiter_n = 0;
Waiter g_cam_waiter{-1, -1};
uint8_t g_cam_seq = 0;
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
  if (g_owner.state() == core::LinkState::kTcpPassthrough) return {"human", "bench"};
  if (g_owner.driverFd() != core::LinkOwner::kNoFd) return {"human", g_ws.name(g_owner.driverFd())};
  return {"none", ""};
}

bool bodyOffline() {
  if (g_owner.state() == core::LinkState::kTcpPassthrough) {
    return millis() - g_link.lastRxMs() >= core::kUnoSilentMs;
  }
  return !g_link.synced();
}

uint32_t tankMs() { return g_link.snapshot().fast_valid ? g_link.snapshot().fast.uno_ms : 0; }

void sendTo(int fd, size_t len) {
  if (len) wsSend(fd, g_json, len);
}

void broadcast(size_t len) {
  if (len == 0) return;
  for (size_t i = 0; i < g_ws.count(); ++i) wsSend(g_ws.fdAt(i), g_json, len);
}

void broadcastTs() {
  broadcast(core::jsonTs(g_json, sizeof(g_json), g_link.snapshot(), ownerView(), bodyOffline()));
}

void nack(int fd, int32_t id, core::BridgeReason r);

// Executes LinkOwner actions in the documented order.
void apply(const core::Actions& a, uint32_t now) {
  if (a.has(core::Actions::kStopMaster)) {
    g_link.stopMaster();
    g_tx.clear();  // drop queued ESP32 frames; a pending STOP (urgent slot) is kept
    // Review M2: a web STOP still being resent when the bench takes over: its ACKs are ours.
    if (g_link.stopPending()) g_tcp.swallowAck(g_link.pendingStopSeq(), now);
    if (g_cam_waiter.fd != -1) {  // stopMaster() cancelled the CAM_HOME: answer it (review M8)
      if (g_ws.contains(g_cam_waiter.fd)) {
        nack(g_cam_waiter.fd, g_cam_waiter.id, core::BridgeReason::kTimeout);
      }
      g_cam_waiter = Waiter{-1, -1};
    }
  }
  if (a.has(core::Actions::kInjectStop)) {
    const uint8_t seq = g_tcp.injectSeq();  // bench lastSeq + 128: no collision with its commands
    g_link.injectStop(seq, now);
    g_tcp.swallowAck(seq, now);
  }
  if (a.has(core::Actions::kSendStop)) g_link.sendStop(now);
  if (a.has(core::Actions::kHintNone)) g_link.setOwnerHint(proto::OwnerHintValue::kNone);
  if (a.has(core::Actions::kHintHuman)) g_link.setOwnerHint(proto::OwnerHintValue::kHuman);
  if (a.has(core::Actions::kStartMaster)) g_link.startMaster(now);
  if (a.has(core::Actions::kOwnerChanged)) {
    broadcast(core::jsonControlChanged(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), ownerView()));
    broadcastTs();
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
    case core::Decision::kNackNotOwner:
    default:
      return core::BridgeReason::kNotOwner;
  }
}

// drive / cam_joy are streaming: a rejection is reported once (nack without id), not per message.
void streamReject(int fd, core::BridgeReason r) {
  if (g_ws.reportStreamReject(fd, static_cast<uint8_t>(r))) nack(fd, -1, r);
}

// SOUND / AUDIO_MODE / CAM_LOCK on the shared acked slot (Plan F, R3.1); false = not queued (body offline).
bool sendAckedCommand(const core::WsCommand& c, uint32_t now, uint8_t& seq) {
  switch (c.kind) {
    case core::WsCmdKind::kSound: return g_link.sendSound(c.sound_id, now, seq);
    case core::WsCmdKind::kAudioMode: return g_link.sendAudioMode(c.audio_mode, now, seq);
    default: return g_link.sendCamLock(c.on, now, seq);
  }
}

void handleWsText(const LinkCommand& cmd, uint32_t now) {
  const int fd = cmd.fd;
  const core::WsCommand c = core::parseWsCommand(cmd.text, cmd.len);
  const int32_t id = c.has_id ? c.id : -1;
  if (c.kind == core::WsCmdKind::kStop) {  // fast path: may overtake its own kWsOpen - never lost
    g_stop_barrier.onStop(cmd.post_seq);
    apply(g_owner.onStop(fd), now);  // urgent slot; queued DRIVE/CAM_JOY frames are purged
    if (g_ws.contains(fd)) {
      g_ws.admit(fd, c.kind, now);  // never limited; counts as alive
      if (g_stop_waiter_n < kMaxStopWaiters) {
        g_stop_waiters[g_stop_waiter_n++] = Waiter{fd, id};
      } else {
        nack(fd, id, core::BridgeReason::kBusy);  // review M8: answered; the STOP was still applied
      }
    }
    return;
  }
  if (!g_ws.contains(fd)) return;
  if ((c.kind == core::WsCmdKind::kDrive || c.kind == core::WsCmdKind::kCamJoy) &&
      g_stop_barrier.stale(cmd.post_seq)) {
    return;  // posted before a STOP that was handled first: silently dropped
  }
  if (!g_ws.admit(fd, c.kind, now)) {
    if (c.kind == core::WsCmdKind::kDrive || c.kind == core::WsCmdKind::kCamJoy) {
      streamReject(fd, core::BridgeReason::kRateLimited);
    } else {
      nack(fd, id, core::BridgeReason::kRateLimited);
    }
    return;
  }
  core::Actions a;
  switch (c.kind) {
    case core::WsCmdKind::kHello: {
      const core::HelloInfo hello = g_link.hello();
      const core::WelcomeInfo w{CQ_FW_VERSION, &hello, g_sensor(), ownerView(), g_ws.name(fd)};
      sendTo(fd, core::jsonWelcome(g_json, sizeof(g_json), w));
      sendTo(fd, core::jsonTs(g_json, sizeof(g_json), g_link.snapshot(), ownerView(), bodyOffline()));
      break;
    }
    case core::WsCmdKind::kDrive: {
      if (g_ota_active) {
        streamReject(fd, core::BridgeReason::kBusy);
        break;
      }
      const core::Decision d = g_owner.onDrive(fd, now, a);
      apply(a, now);
      if (d != core::Decision::kSend) {
        streamReject(fd, reasonOf(d));
      } else if (g_link.sendDrive(c.v, c.w)) {  // 1:1 - the bridge never repeats a DRIVE
        g_ws.streamAccepted(fd);
      } else {
        streamReject(fd, core::BridgeReason::kBodyOffline);
      }
      break;
    }
    case core::WsCmdKind::kStop:  // handled above (fast path)
      break;
    case core::WsCmdKind::kCamJoy: {
      const core::Decision d = g_owner.onCamera(fd);
      if (d != core::Decision::kSend || g_ota_active) {
        streamReject(fd, g_ota_active ? core::BridgeReason::kBusy : reasonOf(d));
      } else if (g_link.sendCamJoy(c.x, c.y, c.active)) {
        g_ws.streamAccepted(fd);
      }
      break;
    }
    case core::WsCmdKind::kCamHome: {
      const core::Decision d = g_owner.onCamera(fd);
      if (d != core::Decision::kSend) {
        nack(fd, id, reasonOf(d));
      } else if (g_cam_waiter.fd != -1) {
        nack(fd, id, core::BridgeReason::kBusy);  // review M8: one cam_home at a time, always answered
      } else if (g_link.sendCamHome(now, g_cam_seq)) {
        g_cam_waiter = Waiter{fd, id};
      } else {
        nack(fd, id, core::BridgeReason::kBodyOffline);
      }
      break;
    }
    case core::WsCmdKind::kSound:
    case core::WsCmdKind::kAudioMode:  // R3.1: as sound
    case core::WsCmdKind::kCamLock: {  // Plan F: owner or nobody (as the camera); acked by the Uno, one at a time
      const core::Decision d = g_owner.onCamera(fd);
      uint8_t seq = 0;
      if (d != core::Decision::kSend || g_ota_active) {
        nack(fd, id, g_ota_active ? core::BridgeReason::kBusy : reasonOf(d));
      } else if (g_cam_waiter.fd != -1) {
        nack(fd, id, core::BridgeReason::kBusy);
      } else if (sendAckedCommand(c, now, seq)) {
        g_cam_seq = seq;
        g_cam_waiter = Waiter{fd, id};
      } else {
        nack(fd, id, core::BridgeReason::kBodyOffline);
      }
      break;
    }
    case core::WsCmdKind::kRelease:
      apply(g_owner.onRelease(fd), now);
      if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
      break;
    case core::WsCmdKind::kPing:
      sendTo(fd, core::jsonPong(g_json, sizeof(g_json), c.t0, now));
      break;
    case core::WsCmdKind::kUnsupported:
    case core::WsCmdKind::kInvalid:
    default:
      nack(fd, id, core::BridgeReason::kInvalidCommand);
      break;
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
      const core::Actions a = g_owner.onWsClose(cmd.fd);
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
      g_ota_active = true;  // from now on: drives nacked BUSY, benches and new phones refused
      if (g_owner.state() == core::LinkState::kTcpPassthrough) {
        g_tx.clear();  // review I3: queued bench frames never follow the injected STOP
        g_tcp.close();
        core::Actions a = g_owner.onTcpClose();  // InjectStop
        a.bits &= static_cast<uint8_t>(~core::Actions::kStartMaster);
        apply(a, now);
      } else {
        g_link.sendStop(now);  // resent until the Uno's ACK
      }
      g_link.stopMaster();  // heartbeats stop: Uno link-loss stop at standstill is harmless
      closeAllClients();
      break;
  }
}

void handleLinkEvents(uint32_t now) {
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) {
    switch (ev.kind) {
      case core::LinkEventKind::kBodyOnline: {
        const core::HelloInfo& h = g_link.hello();
        logf("uno: synced fw %u.%u proto %u commit %08lx", h.fw >> 8, h.fw & 0xFF, h.proto,
             static_cast<unsigned long>(h.commit));
        broadcast(core::jsonBody(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), true, -1));
        apply(g_owner.onSynced(true), now);
        break;
      }
      case core::LinkEventKind::kBodyOffline:
        if (ev.code == 1) {
          logf("uno: incompatible protocol %d (need %u) - flash Plan B firmware", ev.a,
               proto::kProtocolVersion);
        } else {
          logf("uno: body offline (silent)");
        }
        broadcast(core::jsonBody(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), false,
                                 ev.code == 1 ? ev.a : -1));
        apply(g_owner.onSynced(false), now);
        break;
      case core::LinkEventKind::kUnoEvent:
        if (ev.code == static_cast<uint8_t>(proto::EventKind::kControlChanged)) {
          apply(g_owner.onUnoOwner(static_cast<proto::Owner>(ev.a)), now);
        }
        broadcast(core::jsonUnoEvent(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), ev.code, ev.a,
                                     ev.b));
        break;
      case core::LinkEventKind::kStopResult:
        for (size_t i = 0; i < g_stop_waiter_n; ++i) {
          const Waiter& w = g_stop_waiters[i];
          if (!g_ws.contains(w.fd)) continue;
          sendTo(w.fd, ev.status == 0 ? core::jsonAck(g_json, sizeof(g_json), w.id)
                                      : core::jsonNackStatus(g_json, sizeof(g_json), w.id,
                                                             ev.status));
        }
        g_stop_waiter_n = 0;
        break;
      case core::LinkEventKind::kAckResult:
        if (ev.seq == g_cam_seq && g_ws.contains(g_cam_waiter.fd)) {
          sendTo(g_cam_waiter.fd,
                 ev.status == 0
                     ? core::jsonAck(g_json, sizeof(g_json), g_cam_waiter.id)
                     : core::jsonNackStatus(g_json, sizeof(g_json), g_cam_waiter.id, ev.status));
        }
        g_cam_waiter = Waiter{-1, -1};
        break;
      case core::LinkEventKind::kResetRequest:
      default:
        logf("uno: %s reset request ignored (bridge has no stored settings)",
             ev.code == static_cast<uint8_t>(proto::FrameType::kWifiResetReq) ? "WiFi" : "admin");
        break;
    }
  }
}

void telemetry(uint32_t now) {
  const core::TankSnapshot& s = g_link.snapshot();
  if (s.fast_count != g_last_fast_count) {
    g_last_fast_count = s.fast_count;
    if (s.fast_count % 5u == 0) {  // Uno 50 Hz -> page 10 Hz (R2.14 #7, spec 02 §E4)
      broadcast(core::jsonTf(g_json, sizeof(g_json), ++g_tf_seq, s));
    }
  }
  if (now - g_last_ts_ms >= 1000) {  // 1 Hz, also while the Uno is silent (body_offline)
    g_last_ts_ms = now;
    broadcastTs();
  }
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
  s.tcp_active = g_tcp.active();
  s.tcp_refused = g_tcp.stats().refused;
  snprintf(s.last_refusal, sizeof(s.last_refusal), "%s", g_tcp.lastRefusal());
  s.frames_tx = g_link.stats().frames_tx;
  s.frames_rx = g_link.stats().frames_rx;
  s.bad_crc = g_link.parserStats().bad_crc;
  s.tx_dropped = g_tx.dropped();
  s.tcp_up = g_tcp.stats().up;
  s.tcp_down = g_tcp.stats().down;
  portENTER_CRITICAL(&g_status_mux);
  g_status = s;
  portEXIT_CRITICAL(&g_status_mux);
}

void linkTask(void*) {
  uint8_t rx[256];
  uint32_t last_publish = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    size_t n = 0;
    while ((n = g_uart.read(rx, sizeof(rx))) != 0) {
      g_link.onUartBytes(rx, n, now);  // snapshot, events, STOP ACKs - also in passthrough
      g_tcp.forwardFromUno(rx, n, now);
    }
    LinkCommand cmd;
    while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) handleCommand(cmd, now);
    // Only a NEW slow frame updates the owner (review I1): re-applying the 1 Hz frame's older owner
    // every tick would undo an IR takeover reported by EVENT control_changed.
    if (g_link.snapshot().slow_count != g_last_slow_count) {
      g_last_slow_count = g_link.snapshot().slow_count;
      apply(g_owner.onUnoOwner(static_cast<proto::Owner>(g_link.snapshot().slow.owner)), now);
    }
    const char* driver = g_owner.driverFd() != core::LinkOwner::kNoFd
                             ? g_ws.name(g_owner.driverFd())
                             : "a phone";
    apply(g_tcp.poll(now, g_owner, g_tx, driver, g_ota_active), now);
    apply(g_owner.tick(now), now);
    reapSilent(now);
    g_link.step(now);
    handleLinkEvents(now);
    telemetry(now);
    drainTx();
    if (now - last_publish >= 100) {
      last_publish = now;
      publishStatus();
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

}  // namespace

void linkTaskStart(const char* (*camera_sensor)()) {
  g_sensor = camera_sensor;
  g_uart.begin();
  g_tcp.begin();
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
