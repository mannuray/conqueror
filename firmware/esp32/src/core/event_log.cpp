#include "core/event_log.h"

#include <string.h>

namespace cq {
namespace core {

uint32_t EventLog::add(LogKind kind, int32_t a, int32_t b, const char* client, uint32_t uno_ms, uint32_t now_ms) {
  LogEntry& e = m_e[(m_next - 1) % kCap];
  e.seq = m_next;
  e.t_ms = now_ms;
  e.uno_ms = uno_ms;
  e.kind = kind;
  e.a = a;
  e.b = b;
  strncpy(e.client, client != nullptr ? client : "", sizeof(e.client) - 1);
  e.client[sizeof(e.client) - 1] = '\0';
  return m_next++;
}

uint8_t EventLog::since(uint32_t seq, LogEntry* out, uint8_t max, bool& gap) const {
  const uint32_t first_kept = oldest();
  uint32_t s = seq + 1;
  gap = s < first_kept && m_next > kCap;
  if (s < first_kept) s = first_kept;
  uint8_t n = 0;
  for (; s < m_next && n < max; ++s) out[n++] = m_e[(s - 1) % kCap];
  return n;
}

}  // namespace core
}  // namespace cq
