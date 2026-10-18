// WifiPolicy: station first, AP after 10 s (owner 2026-10-16; was 30 s), retry every 60 s only without clients.
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

void test_fallback_ap_after_10s() {
  WifiPolicy p;
  p.begin(0);
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(9999, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kStartAp), A(p.step(10000, false, false)));
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

// Review M9: the home network coming back does not drop the AP under connected phones.
void test_ap_stays_while_clients_connected() {
  WifiPolicy p;
  p.begin(0);
  p.step(30000, false, false);  // fallback AP
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(40000, true, true)));  // phone on the AP
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kFallbackAp), static_cast<int>(p.mode()));
  TEST_ASSERT_EQUAL(A(WifiAction::kApOff), A(p.step(50000, true, false)));  // phone gone
}

void test_station_lost_reconnects() {
  WifiPolicy p;
  p.begin(0);
  p.step(10, true, false);
  TEST_ASSERT_EQUAL(A(WifiAction::kStartStation), A(p.step(5000, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kNone), A(p.step(14999, false, false)));
  TEST_ASSERT_EQUAL(A(WifiAction::kStartAp), A(p.step(15000, false, false)));  // 10 s after the loss
}

// Plan M (bridge LLD R9.3): forced AP by double press / confirmed wifi_mode.
void test_forced_ap_has_no_retries_and_home_restarts() {
  WifiPolicy p;
  p.begin(0);
  p.step(2000, true, false);  // station up
  TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kStartAp), static_cast<int>(p.forceAp(3000)));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kForcedAp), static_cast<int>(p.mode()));
  for (uint32_t t = 3000; t < 300000; t += 1000) {
    TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kNone), static_cast<int>(p.step(t, false, false)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kStartStation), static_cast<int>(p.forceHome(300000)));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kConnecting), static_cast<int>(p.mode()));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kStationUp), static_cast<int>(p.step(302000, true, false)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_station_connects);
  RUN_TEST(test_fallback_ap_after_10s);
  RUN_TEST(test_retry_every_60s_only_without_clients);
  RUN_TEST(test_ap_stays_while_clients_connected);
  RUN_TEST(test_station_lost_reconnects);
  RUN_TEST(test_forced_ap_has_no_retries_and_home_restarts);
  return UNITY_END();
}
