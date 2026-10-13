#include "domain/util/scheduler.h"

namespace cq {

void Scheduler::start(uint32_t now_us) {
  for (uint8_t i = 0; i < m_count; ++i) {
    m_tasks[i].next_due_us = now_us + m_tasks[i].phase_us;
  }
}

uint8_t Scheduler::runDue(uint32_t now_us) {
  uint8_t ran = 0;
  for (uint8_t i = 0; i < m_count; ++i) {
    Task& t = m_tasks[i];
    if (static_cast<int32_t>(now_us - t.next_due_us) < 0) {
      continue;
    }
    t.fn();
    ++ran;
    const uint32_t next = t.next_due_us + t.period_us;
    if (static_cast<int32_t>(now_us - next) >= 0) {
      t.next_due_us = now_us + t.period_us;
      ++m_overruns;
    } else {
      t.next_due_us = next;
    }
  }
  return ran;
}

void LoadMeter::add(uint32_t busy_us, uint32_t now_us) {
  m_busy_us += busy_us;
  if (busy_us > m_max_us) {
    m_max_us = busy_us > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(busy_us);
  }
  const uint32_t window = now_us - m_window_start_us;
  if (window >= 1000000u) {
    const uint32_t pct = (m_busy_us * 100u) / window;
    m_load_pct = pct > 100u ? 100u : static_cast<uint8_t>(pct);
    m_busy_us = 0;
    m_window_start_us = now_us;
  }
}

}  // namespace cq
