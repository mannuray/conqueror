// WifiPolicy: station first, AP after 30 s, retry every 60 s only without clients.
#include <unity.h>

#include "core/wifi_policy.h"

using cq::core::WifiAction;
using cq::core::WifiMode;
using cq::core::WifiPolicy;

namespace {
int A(WifiAction a) { return static_cast<int>(a); }
}  // namespace

void setUp() {}
void tearDown() {}

void test_station_connects() {
  WifiPolicy p;
  TEST_ASSERT_EQUAL(A(WifiAction::kStartStation), A(p.begin(0)));
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(1000, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kStationUp), A(p.step(2000, true, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kStation), static_cast<int>(p.mode()));
}

void test_fallback_ap_after_30s() {
  WifiPolicy p;
  p.begin(0);
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(29999, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kStartAp), A(p.step(30000, false, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kFallbackAp), static_cast<int>(p.mode()));
}

void test_retry_every_60s_only_without_clients() {
  WifiPolicy p;
  p.begin(0);
  p.step(30000, false, false);
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(90000, false, true)));  // phone connected
  TEST_ASSERT_EQUAL(A(WifiAction::kRetryStation), A(p.step(90001, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(100000, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kApOff), A(p.step(110000, true, false)));
}

void test_station_lost_reconnects() {
  WifiPolicy p;
  p.begin(0);
  p.step(10, true, false);
  TEST_ASSERT_EQUAL(A(WifiAction::kStartStation), A(p.step(5000, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kStartAp), A(p.step(35000, false, false)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_station_connects);
  RUN_TEST(test_fallback_ap_after_30s);
  RUN_TEST(test_retry_every_60s_only_without_clients);
  RUN_TEST(test_station_lost_reconnects);
  return UNITY_END();
}
