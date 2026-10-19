// Bridge LLD R13: the event log ring (200), paging with since(), wrap and gap.
#include <string.h>
#include <unity.h>

#include "core/event_log.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

void test_log_add_and_since_pages() {
  EventLog l;
  for (int i = 0; i < 30; ++i) l.add(LogKind::kUno, i, 0, "phone-2", 0, static_cast<uint32_t>(i));
  LogEntry out[25];
  bool gap = false;
  TEST_ASSERT_EQUAL(25, l.since(0, out, 25, gap));
  TEST_ASSERT_EQUAL(1u, out[0].seq);
  TEST_ASSERT_FALSE(gap);
  TEST_ASSERT_EQUAL(5, l.since(25, out, 25, gap));
  TEST_ASSERT_EQUAL(30u, out[4].seq);
  TEST_ASSERT_EQUAL(0, l.since(30, out, 25, gap));
  TEST_ASSERT_EQUAL(30u, l.last());
}

void test_log_since_after_wrap_returns_oldest_kept() {
  EventLog l;
  for (int i = 0; i < 250; ++i) l.add(LogKind::kUno, i, 0, "", 0, 0);
  LogEntry out[25];
  bool gap = false;
  TEST_ASSERT_EQUAL(25, l.since(10, out, 25, gap));
  TEST_ASSERT_TRUE(gap);
  TEST_ASSERT_EQUAL(51u, out[0].seq);  // 250 - 200 + 1
  TEST_ASSERT_EQUAL(51u, l.oldest());
}

void test_log_client_name_truncated_and_terminated() {
  EventLog l;
  l.add(LogKind::kStopHold, 0, 0, "a-very-long-client-name", 0, 0);
  LogEntry out[1];
  bool gap;
  l.since(0, out, 1, gap);
  TEST_ASSERT_EQUAL(11, static_cast<int>(strlen(out[0].client)));
}

void test_log_empty() {
  EventLog l;
  LogEntry out[5];
  bool gap = true;
  TEST_ASSERT_EQUAL(0, l.since(0, out, 5, gap));
  TEST_ASSERT_FALSE(gap);
  TEST_ASSERT_EQUAL(0u, l.last());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_log_add_and_since_pages);
  RUN_TEST(test_log_since_after_wrap_returns_oldest_kept);
  RUN_TEST(test_log_client_name_truncated_and_terminated);
  RUN_TEST(test_log_empty);
  return UNITY_END();
}
