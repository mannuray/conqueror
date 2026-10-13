// BootHistory: boot counter + the last 8 reset reasons (design-notes §38), pure C++.
#include <string.h>
#include <unity.h>

#include "core/boot_history.h"

using cq::core::BootHistory;

void setUp() {}
void tearDown() {}

void test_counts_boots_and_keeps_order() {
  BootHistory h;
  h.push(1);
  h.push(9);
  TEST_ASSERT_EQUAL_UINT32(2, h.count());
  uint8_t r[BootHistory::kSize];
  TEST_ASSERT_EQUAL(2, h.recent(r, BootHistory::kSize));
  TEST_ASSERT_EQUAL(9, r[0]);  // newest first
  TEST_ASSERT_EQUAL(1, r[1]);
}

void test_keeps_only_the_last_eight() {
  BootHistory h;
  for (uint8_t i = 1; i <= 10; ++i) h.push(i);
  uint8_t r[BootHistory::kSize];
  TEST_ASSERT_EQUAL(8, h.recent(r, BootHistory::kSize));
  TEST_ASSERT_EQUAL(10, r[0]);
  TEST_ASSERT_EQUAL(3, r[7]);
  TEST_ASSERT_EQUAL_UINT32(10, h.count());
}

void test_restores_from_stored_bytes() {
  BootHistory a;
  for (uint8_t i = 1; i <= 3; ++i) a.push(i);
  BootHistory b;
  b.restore(a.count(), a.ring(), a.head());
  b.push(7);
  uint8_t r[BootHistory::kSize];
  TEST_ASSERT_EQUAL(4, b.recent(r, BootHistory::kSize));
  TEST_ASSERT_EQUAL(7, r[0]);
  TEST_ASSERT_EQUAL(1, r[3]);
}

void test_reason_names() {
  TEST_ASSERT_EQUAL_STRING("brownout", BootHistory::reasonName(9));
  TEST_ASSERT_EQUAL_STRING("power_on", BootHistory::reasonName(1));
  TEST_ASSERT_EQUAL_STRING("unknown", BootHistory::reasonName(200));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_counts_boots_and_keeps_order);
  RUN_TEST(test_keeps_only_the_last_eight);
  RUN_TEST(test_restores_from_stored_bytes);
  RUN_TEST(test_reason_names);
  return UNITY_END();
}
