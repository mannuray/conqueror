#include "app/replies.h"

#include "ports/client_out.h"
#include "ports/log_out.h"

namespace cq {
namespace app {

void Replies::to(int fd, size_t len) {
  if (len != 0 && m_ws.contains(fd)) ports::clientSend(fd, m_json, len);
}

void Replies::bigTo(int fd, size_t len) {
  if (!m_ws.contains(fd)) return;
  if (len == 0 || !ports::clientSend(fd, m_big, len)) {
    ports::logLine("reply to %s not sent (%u B)", m_ws.name(fd), static_cast<unsigned>(len));
  }
}

void Replies::all(size_t len) {
  if (len == 0) return;
  for (size_t i = 0; i < m_ws.count(); ++i) ports::clientSend(m_ws.fdAt(i), m_json, len);
}

void Replies::ack(int fd, int32_t id) {
  if (id >= 0) to(fd, core::jsonAck(m_json, sizeof(m_json), id));
}

void Replies::nack(int fd, int32_t id, core::BridgeReason r) { to(fd, core::jsonNack(m_json, sizeof(m_json), id, r)); }

void Replies::nackStatus(int fd, int32_t id, uint8_t status) {
  to(fd, core::jsonNackStatus(m_json, sizeof(m_json), id, status));
}

void Replies::streamReject(int fd, core::BridgeReason r) {
  if (m_ws.reportStreamReject(fd, static_cast<uint8_t>(r))) nack(fd, -1, r);
}

}  // namespace app
}  // namespace cq
