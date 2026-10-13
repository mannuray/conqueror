// link_task: the only owner of the Uno UART, the body-link core objects, the link owner and the
// bench TCP client (bridge LLD R2.11). Runs on core 1 every 2 ms. Other tasks talk to it only
// through linkPost() and read a copy of its state with linkStatus().
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/body_link.h"
#include "core/link_owner.h"

namespace cq {
namespace net {

struct LinkCommand {
  enum class Kind : uint8_t { kWsOpen, kWsClose, kWsText, kOtaStart };
  Kind kind;
  int fd;
  uint16_t len;
  char text[200];  // one WS JSON message (longer ones are refused by the WS handler)
};

struct LinkStatus {
  core::LinkState state = core::LinkState::kIdle;
  bool ota_allowed = true;
  bool ota_active = false;
  bool synced = false;
  core::HelloInfo hello;
  char driver[16] = "";   // "phone-2" / "bench" / ""
  uint8_t ws_clients = 0;
  bool tcp_active = false;
  uint32_t tcp_refused = 0;
  char last_refusal[48] = "";
  uint32_t frames_tx = 0;
  uint32_t frames_rx = 0;
  uint32_t bad_crc = 0;
  uint32_t tcp_up = 0;    // bench -> Uno bytes
  uint32_t tcp_down = 0;  // Uno -> bench bytes
};

void linkTaskStart();
bool linkPost(const LinkCommand& cmd, bool urgent);  // urgent (STOP, OTA) goes to the queue front
LinkStatus linkStatus();                             // consistent copy, any task

}  // namespace net
}  // namespace cq
