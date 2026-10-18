// link_task: the only owner of the Uno UART, the body-link core objects and the link owner
// (bridge LLD R2.11; the bench TCP client was removed, R10). Runs on core 1 every 2 ms. Other tasks talk to it only
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
  uint32_t post_seq;  // WS handler's running number (core::StopBarrier); 0 for other kinds
  uint16_t len;
  char text[200];  // one WS JSON message (longer ones are refused by the WS handler)
};

struct LinkStatus {
  core::LinkState state = core::LinkState::kIdle;
  bool ota_allowed = true;
  bool ota_active = false;
  bool synced = false;
  core::HelloInfo hello;
  char driver[16] = "";   // "phone-2" / ""
  uint8_t ws_clients = 0;
  uint32_t frames_tx = 0;
  uint32_t frames_rx = 0;
  uint32_t bad_crc = 0;
  uint32_t tx_dropped = 0;  // chunks refused by a full FrameTx - review M12
};

void linkTaskStart(const char* (*camera_sensor)());  // sensor name for welcome, read when needed
                                                      // (the camera starts after WiFi, §38)
bool linkPost(const LinkCommand& cmd, bool to_front);  // to_front: WS stop (fast path), OTA start
LinkStatus linkStatus();                             // consistent copy, any task

}  // namespace net
}  // namespace cq
