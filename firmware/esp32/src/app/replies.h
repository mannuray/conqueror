// The JSON reply buffers + the ClientOut port, shared by every controller (bridge LLD R15.3).
// Link task only (one owner): the buffers are reused for every reply.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/ws_clients.h"
#include "core/ws_json.h"

namespace cq {
namespace app {

class Replies {
 public:
  explicit Replies(core::WsClients& ws) : m_ws(ws) {}
  char* buf() { return m_json; }
  size_t cap() const { return sizeof(m_json); }
  char* big() { return m_big; }
  size_t bigCap() const { return sizeof(m_big); }
  void to(int fd, size_t len);     // buf() -> fd; nothing when len == 0 or fd is not a client
  void bigTo(int fd, size_t len);  // big() -> fd; logs when it could not be sent
  void all(size_t len);            // buf() -> every client
  void ack(int fd, int32_t id);    // only when id >= 0
  void nack(int fd, int32_t id, core::BridgeReason r);
  void nackStatus(int fd, int32_t id, uint8_t status);
  void streamReject(int fd, core::BridgeReason r);  // drive / cam_joy streams: reported once, not per message
  core::WsClients& clients() { return m_ws; }

 private:
  core::WsClients& m_ws;
  char m_json[core::kWsJsonMax];
  char m_big[core::kWsBigMax];
};

}  // namespace app
}  // namespace cq
