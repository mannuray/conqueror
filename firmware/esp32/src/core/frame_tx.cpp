#include "core/frame_tx.h"

#include <string.h>

namespace cq {
namespace core {

bool FrameTx::push(const uint8_t* data, size_t len, ChunkClass cls) {
  if (len == 0 || len > kMaxChunk || m_count == kSlots) {
    ++m_dropped;
    return false;
  }
  const size_t tail = (m_head + m_count) % kSlots;
  memcpy(m_data[tail], data, len);
  m_len[tail] = static_cast<uint8_t>(len);
  m_cls[tail] = cls;
  ++m_count;
  return true;
}

void FrameTx::purgeMotion() {
  // Keeps the order of the remaining chunks; the chunk being written (if any) always stays.
  const size_t first = (m_inflight == Inflight::kQueue) ? 1 : 0;
  size_t kept = first;
  for (size_t r = first; r < m_count; ++r) {
    const size_t from = (m_head + r) % kSlots;
    if (m_cls[from] == ChunkClass::kMotion) continue;
    const size_t to = (m_head + kept) % kSlots;
    if (to != from) {
      memcpy(m_data[to], m_data[from], m_len[from]);
      m_len[to] = m_len[from];
      m_cls[to] = m_cls[from];
    }
    ++kept;
  }
  m_purged += static_cast<uint32_t>(m_count - kept);
  m_count = kept;
}

bool FrameTx::pushUrgent(const uint8_t* data, size_t len) {
  if (len == 0 || len > kMaxChunk) {
    ++m_dropped;
    return false;
  }
  purgeMotion();  // also when the slot is busy: the resend 50 ms later must not trail a drive
  if (m_inflight == Inflight::kUrgent) {
    ++m_dropped;
    return false;
  }
  memcpy(m_urgent, data, len);
  m_urgent_len = len;
  return true;
}

size_t FrameTx::peek(const uint8_t*& data) {
  if (m_inflight == Inflight::kNone) {
    if (m_urgent_len) {
      m_inflight = Inflight::kUrgent;
    } else if (m_count) {
      m_inflight = Inflight::kQueue;
    } else {
      return 0;
    }
  }
  if (m_inflight == Inflight::kUrgent) {
    data = m_urgent;
    return m_urgent_len;
  }
  data = m_data[m_head];
  return m_len[m_head];
}

void FrameTx::pop() {
  if (m_inflight == Inflight::kUrgent) {
    m_urgent_len = 0;
  } else if (m_inflight == Inflight::kQueue) {
    m_head = (m_head + 1) % kSlots;
    --m_count;
  }
  m_inflight = Inflight::kNone;
}

void FrameTx::clear() {
  if (m_inflight == Inflight::kQueue) {
    m_count = 1;  // keep the chunk being written
  } else {
    m_count = 0;
  }
}

}  // namespace core
}  // namespace cq
