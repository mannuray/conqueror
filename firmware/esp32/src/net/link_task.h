// link_task: the only owner of the Uno UART and the body-link core objects (bridge LLD R2.11).
// Runs on core 1 every 2 ms. Other tasks talk to it only through linkPost().
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

struct LinkCommand {
  enum class Kind : uint8_t { kWsOpen, kWsClose, kWsText, kOtaStart };
  Kind kind;
  int fd;
  uint16_t len;
  char text[200];  // one WS JSON message (longer ones are refused by the WS handler)
};

void linkTaskStart();
bool linkPost(const LinkCommand& cmd, bool urgent);  // urgent (STOP) goes to the queue front

}  // namespace net
}  // namespace cq
