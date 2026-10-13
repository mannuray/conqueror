#pragma once
#include <stdint.h>

namespace cq {

using TaskFn = void (*)();

struct Task {
  TaskFn fn;
  uint32_t period_us;
  uint32_t next_due_us;
};

// Fixed task table, run-to-completion, no catch-up bursts (late task: next = now + period).
class Scheduler {
 public:
  Scheduler(Task* tasks, uint8_t count) : m_tasks(tasks), m_count(count) {}
  void start(uint32_t now_us);
  uint8_t runDue(uint32_t now_us);
  uint16_t overruns() const { return m_overruns; }

 private:
  Task* m_tasks;
  uint8_t m_count;
  uint16_t m_overruns = 0;
};

// Busy time per 1 s window → load %; longest single busy pass.
class LoadMeter {
 public:
  void add(uint32_t busy_us, uint32_t now_us);
  uint8_t loadPct() const { return m_load_pct; }
  uint16_t maxUs() const { return m_max_us; }

 private:
  uint32_t m_window_start_us = 0;
  uint32_t m_busy_us = 0;
  uint8_t m_load_pct = 0;
  uint16_t m_max_us = 0;
};

}  // namespace cq
