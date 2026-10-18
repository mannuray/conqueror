// Plan M (bridge LLD R9.3): confirmation by the tank's button, double press.
#include <string.h>
#include <unity.h>

#include "core/confirm.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

namespace {
PendingChange change(ChangeKind k, int fd) {
  PendingChange c;
  c.kind = k;
  c.fd = fd;
  return c;
}
int A(ButtonAction a) { return static_cast<int>(a); }
}  // namespace

void test_gate_confirm_then_empty() {
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiSave, 7), 0);
  TEST_ASSERT_TRUE(g.waiting());
  PendingChange out;
  TEST_ASSERT_TRUE(g.take(out));
  TEST_ASSERT_EQUAL(static_cast<int>(ChangeKind::kWifiSave), static_cast<int>(out.kind));
  TEST_ASSERT_EQUAL_INT(7, out.fd);
  TEST_ASSERT_FALSE(g.waiting());
  TEST_ASSERT_FALSE(g.take(out));
}

void test_gate_expires_once() {
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiMode, 3), 1000);
  PendingChange out;
  TEST_ASSERT_FALSE(g.expired(1000 + ConfirmGate::kWindowMs - 1, out));
  TEST_ASSERT_TRUE(g.expired(1000 + ConfirmGate::kWindowMs, out));
  TEST_ASSERT_EQUAL_INT(3, out.fd);
  TEST_ASSERT_FALSE(g.expired(1000 + ConfirmGate::kWindowMs + 5000, out));
  TEST_ASSERT_FALSE(g.waiting());
}

void test_gate_new_change_replaces() {
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiSave, 3), 0);
  g.begin(change(ChangeKind::kApPassword, 4), 100);
  PendingChange out;
  TEST_ASSERT_TRUE(g.take(out));
  TEST_ASSERT_EQUAL(static_cast<int>(ChangeKind::kApPassword), static_cast<int>(out.kind));
  TEST_ASSERT_EQUAL_INT(4, out.fd);
}

void test_gate_drop_client() {  // Review Focus 4
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiForget, 9), 0);
  g.dropClient(8);  // someone else
  TEST_ASSERT_TRUE(g.waiting());
  g.dropClient(9);
  TEST_ASSERT_FALSE(g.waiting());
}

void test_double_press_gap() {
  DoublePress d;
  TEST_ASSERT_FALSE(d.onPress(0));
  TEST_ASSERT_TRUE(d.onPress(DoublePress::kGapMs));  // second within 1 s
  TEST_ASSERT_FALSE(d.onPress(5000));
  TEST_ASSERT_FALSE(d.onPress(5000 + DoublePress::kGapMs + 1));  // too slow: this one starts afresh
  TEST_ASSERT_TRUE(d.onPress(5000 + DoublePress::kGapMs + 400));
}

void test_double_press_three_presses_one_toggle() {  // Review Focus 3
  DoublePress d;
  TEST_ASSERT_FALSE(d.onPress(0));
  TEST_ASSERT_TRUE(d.onPress(300));
  TEST_ASSERT_FALSE(d.onPress(600));  // a third press starts a new double, never a second toggle
  TEST_ASSERT_TRUE(d.onPress(900));
}

void test_router_confirms_while_waiting_and_never_doubles() {
  ConfirmGate g;
  ButtonRouter r(g);
  g.begin(change(ChangeKind::kWifiSave, 1), 0);
  TEST_ASSERT_EQUAL(A(ButtonAction::kConfirm), A(r.onPress(100)));  // confirms (the caller takes the change)
  PendingChange out;
  g.take(out);
  TEST_ASSERT_EQUAL(A(ButtonAction::kNone), A(r.onPress(400)));      // the confirming press never counts
  TEST_ASSERT_EQUAL(A(ButtonAction::kToggleWifi), A(r.onPress(700)));  // 400 + 700: a fresh double
}

void test_change_names() {
  TEST_ASSERT_EQUAL_STRING("wifi_save", changeName(ChangeKind::kWifiSave));
  TEST_ASSERT_EQUAL_STRING("wifi_mode", changeName(ChangeKind::kWifiMode));
  TEST_ASSERT_EQUAL_STRING("setting", changeName(ChangeKind::kSetting));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gate_confirm_then_empty);
  RUN_TEST(test_gate_expires_once);
  RUN_TEST(test_gate_new_change_replaces);
  RUN_TEST(test_gate_drop_client);
  RUN_TEST(test_double_press_gap);
  RUN_TEST(test_double_press_three_presses_one_toggle);
  RUN_TEST(test_router_confirms_while_waiting_and_never_doubles);
  RUN_TEST(test_change_names);
  return UNITY_END();
}
