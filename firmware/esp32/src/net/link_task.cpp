#include "net/link_task.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "core/frame_tx.h"
#include "net/log.h"
#include "net/tcp_passthrough.h"
#include "net/uart_port.h"

namespace cq {
namespace net {
namespace {

QueueHandle_t g_queue = nullptr;
portMUX_TYPE g_status_mux = portMUX_INITIALIZER_UNLOCKED;
LinkStatus g_status;

UartPort g_uart;
core::FrameTx g_tx;
core::BodyLink g_link(g_tx);
core::LinkOwner g_owner;
TcpPassthrough g_tcp;
bool g_ota_active = false;

void drainTx() {
  const uint8_t* p = nullptr;
  size_t len = 0;
  while ((len = g_tx.peek(p)) != 0) {
    if (g_uart.txFree() < len) return;  // next tick (UART drains 25 B/ms); never a partial chunk
    g_uart.write(p, len);
    g_tx.pop();
  }
}

// Executes LinkOwner actions in the documented order.
void apply(const core::Actions& a, uint32_t now) {
  if (a.has(core::Actions::kStopMaster)) {
    g_link.stopMaster();
    g_tx.clear();  // drop queued ESP32 frames; a pending STOP (urgent slot) is kept
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
}

void handleLinkEvents(uint32_t now) {
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) {
    switch (ev.kind) {
      case core::LinkEventKind::kBodyOnline: {
        const core::HelloInfo& h = g_link.hello();
        logf("uno: synced fw %u.%u proto %u commit %08lx", h.fw >> 8, h.fw & 0xFF, h.proto,
             static_cast<unsigned long>(h.commit));
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
        apply(g_owner.onSynced(false), now);
        break;
      case core::LinkEventKind::kUnoEvent:
        if (ev.code == static_cast<uint8_t>(proto::EventKind::kControlChanged)) {
          apply(g_owner.onUnoOwner(static_cast<proto::Owner>(ev.a)), now);
        }
        break;
      case core::LinkEventKind::kResetRequest:
        logf("uno: %s reset request ignored (bridge has no stored settings)",
             ev.code == static_cast<uint8_t>(proto::FrameType::kWifiResetReq) ? "WiFi" : "admin");
        break;
      case core::LinkEventKind::kStopResult:
      case core::LinkEventKind::kAckResult:
      default:
        break;
    }
  }
}

void handleCommand(const LinkCommand& cmd, uint32_t now) {
  if (cmd.kind == LinkCommand::Kind::kOtaStart) {
    g_ota_active = true;  // from now on: drives nacked BUSY, benches refused
    if (g_owner.state() == core::LinkState::kTcpPassthrough) {
      g_tcp.close();
      core::Actions a = g_owner.onTcpClose();  // InjectStop
      a.bits &= static_cast<uint8_t>(~core::Actions::kStartMaster);
      apply(a, now);
    } else {
      g_link.sendStop(now);  // resent until the Uno's ACK
    }
    g_link.stopMaster();  // heartbeats stop: Uno link-loss stop at standstill is harmless
  }
}

void publishStatus() {
  LinkStatus s;
  s.state = g_owner.state();
  s.ota_active = g_ota_active;
  s.ota_allowed = g_owner.otaAllowed() && !g_ota_active;
  s.synced = g_link.synced();
  s.hello = g_link.hello();
  s.ws_clients = g_owner.wsCount();
  s.tcp_active = g_tcp.active();
  s.tcp_refused = g_tcp.stats().refused;
  snprintf(s.last_refusal, sizeof(s.last_refusal), "%s", g_tcp.lastRefusal());
  s.tcp_up = g_tcp.stats().up;
  s.tcp_down = g_tcp.stats().down;
  if (s.state == core::LinkState::kTcpPassthrough) snprintf(s.driver, sizeof(s.driver), "bench");
  s.frames_tx = g_link.stats().frames_tx;
  s.frames_rx = g_link.stats().frames_rx;
  s.bad_crc = g_link.parserStats().bad_crc;
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
    if (g_link.snapshot().slow_valid) {
      apply(g_owner.onUnoOwner(static_cast<proto::Owner>(g_link.snapshot().slow.owner)), now);
    }
    apply(g_tcp.poll(now, g_owner, g_tx, "a phone", g_ota_active), now);
    apply(g_owner.tick(now), now);
    g_link.step(now);
    handleLinkEvents(now);
    drainTx();
    if (now - last_publish >= 100) {
      last_publish = now;
      publishStatus();
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

}  // namespace

void linkTaskStart() {
  g_uart.begin();
  g_tcp.begin();
  g_queue = xQueueCreate(16, sizeof(LinkCommand));
  xTaskCreatePinnedToCore(linkTask, "link", 6144, nullptr, 5, nullptr, 1);
  logf("uno link: Serial1 RX %d TX %d %lu baud", board::kUnoRxPin, board::kUnoTxPin,
       static_cast<unsigned long>(board::kUnoBaud));
}

bool linkPost(const LinkCommand& cmd, bool urgent) {
  if (g_queue == nullptr) return false;
  return (urgent ? xQueueSendToFront(g_queue, &cmd, 0) : xQueueSendToBack(g_queue, &cmd, 0)) ==
         pdTRUE;
}

LinkStatus linkStatus() {
  portENTER_CRITICAL(&g_status_mux);
  const LinkStatus s = g_status;
  portEXIT_CRITICAL(&g_status_mux);
  return s;
}

}  // namespace net
}  // namespace cq
