#include "domain/comms/link_session.h"

#include "domain/util/math_util.h"

namespace cq {

bool LinkSession::alive(uint32_t now_ms) const {
  return m_seen && !elapsed(now_ms, m_last_rx_ms, static_cast<uint32_t>(kLinkTimeoutMs) + 1);
}

LinkChange LinkSession::poll(uint32_t now_ms) {
  const bool now_alive = alive(now_ms);
  const bool was_alive = m_was_alive;
  m_was_alive = now_alive;
  if (was_alive && !now_alive) {
    onLinkLost();
    return LinkChange::kLost;
  }
  if (!was_alive && now_alive) {
    const bool first_contact = !m_ever_alive;
    m_ever_alive = true;
    return first_contact ? LinkChange::kNone : LinkChange::kRestored;
  }
  return LinkChange::kNone;
}

bool LinkSession::helloDue(uint32_t now_ms) {
  if (m_hello_due || (!m_synced && elapsed(now_ms, m_last_hello_ms, kRepeatMs))) {
    m_hello_due = false;
    m_last_hello_ms = now_ms;
    return true;
  }
  return false;
}

bool LinkSession::requestDue(uint32_t now_ms, uint8_t& frame_type) {
  if (m_req_type == 0) return false;
  if (m_req_sent && !elapsed(now_ms, m_req_last_ms, kRepeatMs)) return false;
  frame_type = m_req_type;
  return true;
}

}  // namespace cq
