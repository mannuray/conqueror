#include <unity.h>

#include "domain/util/motion_timer.h"
#include "domain/util/scheduler.h"

namespace {
int g_a = 0;
int g_b = 0;
void taskA() { ++g_a; }
void taskB() { ++g_b; }
}  // namespace

void setUp() { g_a = 0; g_b = 0; }
void tearDown() {}

void test_runs_each_task_at_its_period() {
  cq::Task tasks[] = {{taskA, 10000, 0, 0}, {taskB, 20000, 0, 0}};
  cq::Scheduler s(tasks, 2);
  s.start(0);
  for (uint32_t t = 0; t <= 100000; t += 1000) {
    s.runDue(t);
  }
  TEST_ASSERT_EQUAL(11, g_a);
  TEST_ASSERT_EQUAL(6, g_b);
}

void test_overrun_reschedules_from_now_without_bursts() {
  cq::Task tasks[] = {{taskA, 10000, 0, 0}};
  cq::Scheduler s(tasks, 1);
  s.start(0);
  s.runDue(0);
  s.runDue(55000);  // very late: runs once, next due = 65000
  s.runDue(60000);
  TEST_ASSERT_EQUAL(2, g_a);
  TEST_ASSERT_EQUAL(1, s.overruns());
  s.runDue(65000);
  TEST_ASSERT_EQUAL(3, g_a);
}

void test_wraparound_is_safe() {
  cq::Task tasks[] = {{taskA, 10000, 0, 0}};
  cq::Scheduler s(tasks, 1);
  const uint32_t near_wrap = 0xFFFFFFFFu - 4000u;
  s.start(near_wrap);
  s.runDue(near_wrap);
  s.runDue(near_wrap + 10000u);  // wraps past zero
  TEST_ASSERT_EQUAL(2, g_a);
}

void test_phase_offsets_first_run_and_keeps_period() {
  cq::Task tasks[] = {{taskA, 10000, 0, 0}, {taskB, 10000, 2500, 0}};
  cq::Scheduler s(tasks, 2);
  s.start(1000);
  s.runDue(1000);  // A due now; B not until 3500
  TEST_ASSERT_EQUAL(1, g_a);
  TEST_ASSERT_EQUAL(0, g_b);
  s.runDue(3500);
  TEST_ASSERT_EQUAL(1, g_a);
  TEST_ASSERT_EQUAL(1, g_b);
  s.runDue(11000);  // A's slot; B (13500) stays apart
  TEST_ASSERT_EQUAL(2, g_a);
  TEST_ASSERT_EQUAL(1, g_b);
  s.runDue(13500);
  TEST_ASSERT_EQUAL(2, g_b);
  TEST_ASSERT_EQUAL(0, s.overruns());
}

void test_motion_timer_not_moving_at_boot() {
  cq::MotionTimer m;
  TEST_ASSERT_FALSE(m.moving(0));    // never moved: not moving, even in the first 300 ms
  TEST_ASSERT_FALSE(m.moving(100));
  m.mark(1000);
  TEST_ASSERT_TRUE(m.moving(1000));
  TEST_ASSERT_TRUE(m.moving(1299));
  TEST_ASSERT_FALSE(m.moving(1300));  // 300 ms after the last motion
}

void test_load_meter() {
  cq::LoadMeter m;
  m.add(250000, 500000);
  m.add(250000, 1000000);  // window closes: 500 ms busy of 1000 ms
  TEST_ASSERT_EQUAL(50, m.loadPct());
  TEST_ASSERT_EQUAL(0xFFFF, m.maxUs());  // 250 ms clamps to the u16 maximum
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_runs_each_task_at_its_period);
  RUN_TEST(test_overrun_reschedules_from_now_without_bursts);
  RUN_TEST(test_wraparound_is_safe);
  RUN_TEST(test_phase_offsets_first_run_and_keeps_period);
  RUN_TEST(test_motion_timer_not_moving_at_boot);
  RUN_TEST(test_load_meter);
  return UNITY_END();
}
