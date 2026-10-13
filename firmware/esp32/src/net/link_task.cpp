#include "net/link_task.h"

#include <Arduino.h>

#include "board_config.h"
#include "core/body_link.h"
#include "core/frame_tx.h"
#include "net/log.h"
#include "net/uart_port.h"

namespace cq {
namespace net {
namespace {

QueueHandle_t g_queue = nullptr;
UartPort g_uart;
core::FrameTx g_tx;
core::BodyLink g_link(g_tx);

void drainTx() {
  const uint8_t* p = nullptr;
  size_t len = 0;
  while ((len = g_tx.peek(p)) != 0) {
    if (g_uart.txFree() < len) return;  // next tick (UART drains 25 B/ms); never a partial chunk
    g_uart.write(p, len);
    g_tx.pop();
  }
}

void logEvents() {
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) {
    if (ev.kind == core::LinkEventKind::kBodyOnline) {
      const core::HelloInfo& h = g_link.hello();
      logf("uno: synced fw %u.%u proto %u commit %08lx", h.fw >> 8, h.fw & 0xFF, h.proto,
           static_cast<unsigned long>(h.commit));
    } else if (ev.kind == core::LinkEventKind::kBodyOffline) {
      if (ev.code == 1) {
        logf("uno: incompatible protocol %d (need %u) - flash Plan B firmware", ev.a,
             proto::kProtocolVersion);
      } else {
        logf("uno: body offline (silent)");
      }
    } else if (ev.kind == core::LinkEventKind::kResetRequest) {
      logf("uno: %s reset request ignored (bridge has no stored settings)",
           ev.code == static_cast<uint8_t>(proto::FrameType::kWifiResetReq) ? "WiFi" : "admin");
    }
  }
}

void linkTask(void*) {
  uint8_t rx[256];
  bool hello_logged = false;
#ifdef CQ_MASTER_AT_BOOT
  g_link.startMaster(millis());  // Task 2 hardware check only
#endif
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    size_t n = 0;
    while ((n = g_uart.read(rx, sizeof(rx))) != 0) g_link.onUartBytes(rx, n, now);
    if (!hello_logged && g_link.hello().seen) {
      hello_logged = true;
      const core::HelloInfo& h = g_link.hello();
      logf("uno: HELLO fw %u.%u proto %u caps 0x%02x commit %08lx", h.fw >> 8, h.fw & 0xFF,
           h.proto, h.caps, static_cast<unsigned long>(h.commit));
    }
    LinkCommand cmd;
    while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) {
      (void)cmd;  // clients arrive in Tasks 4 and 6
    }
    g_link.step(now);
    drainTx();
    logEvents();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

}  // namespace

void linkTaskStart() {
  g_uart.begin();
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

}  // namespace net
}  // namespace cq
