#include "domain/comms/event_queue.h"

#include "domain/util/math_util.h"

namespace cq {

bool EventQueue::push(proto::EventKind kind, int16_t a, int16_t b) {
  if (m_count == kSize) {
    ++m_dropped;
    return false;
  }
  const uint8_t idx = static_cast<uint8_t>((m_head + m_count) % kSize);
  m_ring[idx] = Event{static_cast<uint8_t>(kind), a, b};
  ++m_count;
  return true;
}

bool EventQueue::peek(Event& out) const {
  if (m_count == 0) return false;
  out = m_ring[m_head];
  return true;
}

bool EventQueue::pop(Event& out) {
  if (m_count == 0) return false;
  out = m_ring[m_head];
  m_head = static_cast<uint8_t>((m_head + 1) % kSize);
  --m_count;
  return true;
}

bool AckQueue::push(uint8_t seq, proto::AckStatus status) {
  if (m_count == kSize) return false;
  const uint8_t idx = static_cast<uint8_t>((m_head + m_count) % kSize);
  m_seq[idx] = seq;
  m_status[idx] = status;
  ++m_count;
  return true;
}

bool AckQueue::peek(uint8_t& seq, proto::AckStatus& status) const {
  if (m_count == 0) return false;
  seq = m_seq[m_head];
  status = m_status[m_head];
  return true;
}

bool AckQueue::pop(uint8_t& seq, proto::AckStatus& status) {
  if (m_count == 0) return false;
  seq = m_seq[m_head];
  status = m_status[m_head];
  m_head = static_cast<uint8_t>((m_head + 1) % kSize);
  --m_count;
  return true;
}

bool RejectLimiter::allow(uint8_t source, uint32_t now_ms) {
  if (source > 2) source = 0;
  if (m_seen[source] && !elapsed(now_ms, m_last_ms[source], 500)) return false;
  m_seen[source] = true;
  m_last_ms[source] = now_ms;
  return true;
}

}  // namespace cq
