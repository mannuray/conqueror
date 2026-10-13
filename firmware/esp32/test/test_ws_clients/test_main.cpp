// WsClients: names, capacity, rate limits, silence reaping, one nack per streaming rejection.
#include <unity.h>

#include "core/ws_clients.h"

using cq::core::StopBarrier;
using cq::core::WsClients;
using cq::core::WsCmdKind;

namespace {
WsClients c;
}

void setUp() { c = WsClients(); }
void tearDown() {}

void test_names_and_capacity() {
  TEST_ASSERT_TRUE(c.add(10, 0));
  TEST_ASSERT_TRUE(c.add(11, 0));
  TEST_ASSERT_EQUAL_STRING("phone-1", c.name(10));
  TEST_ASSERT_EQUAL_STRING("phone-2", c.name(11));
  TEST_ASSERT_TRUE(c.add(12, 0));
  TEST_ASSERT_TRUE(c.add(13, 0));
  TEST_ASSERT_FALSE(c.add(14, 0));  // max 4
  TEST_ASSERT_TRUE(c.remove(10));
  TEST_ASSERT_FALSE(c.remove(10));  // plain HTTP sockets also close: not ours
  TEST_ASSERT_TRUE(c.add(14, 0));
  TEST_ASSERT_EQUAL_STRING("phone-5", c.name(14));  // numbers are never reused
}

void test_drive_rate_limit_25_per_second() {
  c.add(10, 0);
  int accepted = 0;
  for (int i = 0; i < 40; ++i) accepted += c.admit(10, WsCmdKind::kDrive, 10 + i) ? 1 : 0;
  TEST_ASSERT_EQUAL(25, accepted);
  TEST_ASSERT_TRUE(c.admit(10, WsCmdKind::kStop, 60));         // STOP is never limited
  TEST_ASSERT_TRUE(c.admit(10, WsCmdKind::kCamJoy, 61));       // separate budget
  TEST_ASSERT_TRUE(c.admit(10, WsCmdKind::kDrive, 1000));      // next window
}

void test_total_limit() {
  c.add(10, 0);
  int accepted = 0;
  for (int i = 0; i < 80; ++i) accepted += c.admit(10, WsCmdKind::kPing, i) ? 1 : 0;
  TEST_ASSERT_EQUAL(60, accepted);
}

void test_silent_clients_are_reported() {
  c.add(10, 0);
  c.add(11, 0);
  c.admit(11, WsCmdKind::kPing, 2500);
  int fds[4];
  TEST_ASSERT_EQUAL(0, c.silent(2999, fds, 4));
  TEST_ASSERT_EQUAL(1, c.silent(3000, fds, 4));
  TEST_ASSERT_EQUAL(10, fds[0]);
}

// Review M3: a silent client is reported once (closing), not on every 2 ms tick until kWsClose.
void test_silent_client_reported_once() {
  c.add(10, 0);
  int fds[4];
  TEST_ASSERT_EQUAL(1, c.silent(3000, fds, 4));
  TEST_ASSERT_EQUAL(0, c.silent(3002, fds, 4));
  TEST_ASSERT_EQUAL(0, c.silent(9000, fds, 4));
}

void test_stream_reject_reported_once() {
  c.add(10, 0);
  TEST_ASSERT_TRUE(c.reportStreamReject(10, 1));
  TEST_ASSERT_FALSE(c.reportStreamReject(10, 1));  // 20 Hz drive: one nack, not twenty
  TEST_ASSERT_TRUE(c.reportStreamReject(10, 2));   // a new reason is reported
  c.streamAccepted(10);
  TEST_ASSERT_TRUE(c.reportStreamReject(10, 2));
}

void test_stop_barrier_drops_motion_posted_before_stop() {
  StopBarrier b;
  TEST_ASSERT_FALSE(b.stale(5));  // no STOP yet
  b.onStop(10);                   // posted 10th, handled first (queue front)
  TEST_ASSERT_TRUE(b.stale(8));   // drives posted before it, still queued behind it
  TEST_ASSERT_TRUE(b.stale(9));
  TEST_ASSERT_FALSE(b.stale(11));  // the driver pushes the stick again after the STOP
  b.onStop(7);                     // an older STOP handled late never lowers the barrier
  TEST_ASSERT_TRUE(b.stale(9));
  TEST_ASSERT_EQUAL(3, b.dropped());
}

void test_stop_barrier_survives_counter_wrap() {
  StopBarrier b;
  b.onStop(2);                         // counter wrapped: 0xFFFFFFFE was posted before 2
  TEST_ASSERT_TRUE(b.stale(0xFFFFFFFEu));
  TEST_ASSERT_FALSE(b.stale(3));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_names_and_capacity);
  RUN_TEST(test_drive_rate_limit_25_per_second);
  RUN_TEST(test_total_limit);
  RUN_TEST(test_silent_clients_are_reported);
  RUN_TEST(test_silent_client_reported_once);
  RUN_TEST(test_stream_reject_reported_once);
  RUN_TEST(test_stop_barrier_drops_motion_posted_before_stop);
  RUN_TEST(test_stop_barrier_survives_counter_wrap);
  return UNITY_END();
}
