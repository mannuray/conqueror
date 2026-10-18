// Plan M (bridge LLD R9.3): confirmation by the tank's button, double press.
#include <string.h>
#include <unity.h>

#include "core/acked_ops.h"
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


void test_acked_ops_one_at_a_time() {
  AckedOps q;
  AckedOp a{};
  a.kind = AckedOp::kSound;
  AckedOp b{};
  b.kind = AckedOp::kLed;
  TEST_ASSERT_TRUE(q.push(a));
  TEST_ASSERT_TRUE(q.push(b));
  AckedOp f{};
  TEST_ASSERT_TRUE(q.front(f));
  TEST_ASSERT_EQUAL(AckedOp::kSound, f.kind);
  TEST_ASSERT_FALSE(q.busy());
  q.markSent(42, 0);
  TEST_ASSERT_TRUE(q.busy());
  TEST_ASSERT_TRUE(q.isCurrent(42));
  TEST_ASSERT_FALSE(q.isCurrent(43));
  q.done();  // ACK (or timeout) for 42: next op
  TEST_ASSERT_FALSE(q.busy());
  TEST_ASSERT_TRUE(q.front(f));
  TEST_ASSERT_EQUAL(AckedOp::kLed, f.kind);
  q.markSent(43, 10);
  q.done();
  TEST_ASSERT_TRUE(q.empty());
  for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(q.push(a));
  TEST_ASSERT_FALSE(q.push(a));  // full
}


// Plan M review I1: an op whose answer never comes (frame lost) must not block the queue forever.
void test_acked_ops_time_out() {
  AckedOps q;
  AckedOp a{};
  q.push(a);
  q.markSent(5, 1000);
  TEST_ASSERT_FALSE(q.timedOut(1000 + AckedOps::kTimeoutMs - 1));
  TEST_ASSERT_TRUE(q.timedOut(1000 + AckedOps::kTimeoutMs));
  q.done();
  TEST_ASSERT_FALSE(q.busy());
  TEST_ASSERT_FALSE(q.timedOut(999999));  // nothing in flight
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
  RUN_TEST(test_acked_ops_one_at_a_time);
  RUN_TEST(test_acked_ops_time_out);
  return UNITY_END();
}
