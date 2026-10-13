// LinkOwner: every transition of the bridge LLD R2.7 state diagram.
#include <unity.h>

#include "core/link_owner.h"

using cq::core::Actions;
using cq::core::Admit;
using cq::core::Decision;
using cq::core::LinkOwner;
using cq::core::LinkState;
using cq::proto::Owner;

namespace {

LinkOwner o;
constexpr int kA = 11;  // phone A socket
constexpr int kB = 12;  // phone B socket

int st() { return static_cast<int>(o.state()); }
int S(LinkState s) { return static_cast<int>(s); }
int D(Decision d) { return static_cast<int>(d); }

void readyWithA() {
  o.onWsOpen(kA);
  o.onSynced(true);
}

void drivingA(uint32_t now = 100) {
  readyWithA();
  Actions a;
  o.onDrive(kA, now, a);
}

}  // namespace

void setUp() { o = LinkOwner(); }
void tearDown() {}

void test_first_ws_starts_master() {
  const Actions a = o.onWsOpen(kA);
  TEST_ASSERT_TRUE(a.has(Actions::kStartMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kWebHandshake), st());
  TEST_ASSERT_FALSE(o.onWsOpen(kB).has(Actions::kStartMaster));  // second client: no new master
  TEST_ASSERT_EQUAL(2, o.wsCount());
}

void test_drive_before_sync_is_body_offline() {
  o.onWsOpen(kA);
  Actions a;
  TEST_ASSERT_EQUAL(D(Decision::kNackBodyOffline), D(o.onDrive(kA, 0, a)));
}

void test_first_drive_takes_ownership() {
  readyWithA();
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
  Actions a;
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 100, a)));
  TEST_ASSERT_TRUE(a.has(Actions::kHintHuman));
  TEST_ASSERT_TRUE(a.has(Actions::kOwnerChanged));
  TEST_ASSERT_EQUAL(S(LinkState::kWebDriving), st());
  TEST_ASSERT_EQUAL(kA, o.driverFd());
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 150, a)));
  TEST_ASSERT_FALSE(a.has(Actions::kOwnerChanged));  // already the owner
}

void test_other_phone_gets_not_owner() {
  drivingA();
  o.onWsOpen(kB);
  Actions a;
  TEST_ASSERT_EQUAL(D(Decision::kNackNotOwner), D(o.onDrive(kB, 120, a)));
  TEST_ASSERT_EQUAL(D(Decision::kNackNotOwner), D(o.onCamera(kB)));
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onCamera(kA)));
}

void test_handback_after_5s_without_drive() {
  drivingA(100);
  TEST_ASSERT_EQUAL(0, o.tick(5099).bits);
  const Actions a = o.tick(5100);
  TEST_ASSERT_TRUE(a.has(Actions::kSendStop));
  TEST_ASSERT_TRUE(a.has(Actions::kHintNone));
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
  TEST_ASSERT_EQUAL(LinkOwner::kNoFd, o.driverFd());
}

void test_release_control() {
  drivingA();
  TEST_ASSERT_EQUAL(0, o.onRelease(kB).bits);  // not the driver: ignored
  const Actions a = o.onRelease(kA);
  TEST_ASSERT_TRUE(a.has(Actions::kSendStop));
  TEST_ASSERT_TRUE(a.has(Actions::kOwnerChanged));
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
}

void test_driver_close_with_others_left_stops_and_keeps_master() {
  drivingA();
  o.onWsOpen(kB);
  const Actions a = o.onWsClose(kA);
  TEST_ASSERT_TRUE(a.has(Actions::kSendStop));
  TEST_ASSERT_FALSE(a.has(Actions::kStopMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
}

void test_last_ws_close_stops_and_goes_idle() {
  drivingA();
  const Actions a = o.onWsClose(kA);
  TEST_ASSERT_TRUE(a.has(Actions::kSendStop));
  TEST_ASSERT_TRUE(a.has(Actions::kStopMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kIdle), st());
}

void test_stop_from_anyone_in_every_state() {
  TEST_ASSERT_TRUE(o.onStop(kB).has(Actions::kSendStop));  // Idle
  drivingA();
  o.onWsOpen(kB);
  TEST_ASSERT_TRUE(o.onStop(kB).has(Actions::kSendStop));  // not the driver: still accepted
  TEST_ASSERT_EQUAL(kA, o.driverFd());                      // STOP does not change the owner
}

void test_bench_connect_from_idle_and_ready() {
  Actions a;
  TEST_ASSERT_EQUAL(static_cast<int>(Admit::kAccept), static_cast<int>(o.onTcpConnect(a)));
  TEST_ASSERT_FALSE(a.has(Actions::kStopMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kTcpPassthrough), st());
  o = LinkOwner();
  readyWithA();
  TEST_ASSERT_EQUAL(static_cast<int>(Admit::kAccept), static_cast<int>(o.onTcpConnect(a)));
  TEST_ASSERT_TRUE(a.has(Actions::kStopMaster));  // web master ends at a frame boundary
  TEST_ASSERT_TRUE(a.has(Actions::kOwnerChanged));
}

void test_bench_refused_while_phone_drives() {
  drivingA();
  Actions a;
  TEST_ASSERT_EQUAL(static_cast<int>(Admit::kRefuseBusy), static_cast<int>(o.onTcpConnect(a)));
  TEST_ASSERT_EQUAL(0, a.bits);
  TEST_ASSERT_EQUAL(S(LinkState::kWebDriving), st());
}

void test_second_bench_refused() {
  Actions a;
  o.onTcpConnect(a);
  TEST_ASSERT_EQUAL(static_cast<int>(Admit::kRefuseBusy), static_cast<int>(o.onTcpConnect(a)));
}

void test_phone_during_passthrough_gets_busy_and_stop_is_injected() {
  Actions a;
  o.onTcpConnect(a);
  o.onWsOpen(kA);
  TEST_ASSERT_EQUAL(S(LinkState::kTcpPassthrough), st());
  TEST_ASSERT_EQUAL(D(Decision::kNackBusy), D(o.onDrive(kA, 10, a)));
  TEST_ASSERT_EQUAL(D(Decision::kNackBusy), D(o.onCamera(kA)));
  const Actions s = o.onStop(kA);
  TEST_ASSERT_TRUE(s.has(Actions::kInjectStop));
  TEST_ASSERT_FALSE(s.has(Actions::kSendStop));
}

void test_bench_close_with_ws_clients_rehandshakes() {
  Actions a;
  o.onTcpConnect(a);
  o.onWsOpen(kA);
  const Actions c = o.onTcpClose();
  TEST_ASSERT_TRUE(c.has(Actions::kInjectStop));
  TEST_ASSERT_TRUE(c.has(Actions::kStartMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kWebHandshake), st());
}

void test_bench_close_alone_goes_idle() {
  Actions a;
  o.onTcpConnect(a);
  const Actions c = o.onTcpClose();
  TEST_ASSERT_TRUE(c.has(Actions::kInjectStop));
  TEST_ASSERT_FALSE(c.has(Actions::kStartMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kIdle), st());
}

void test_ir_beats_phone() {
  drivingA();
  const Actions a = o.onUnoOwner(Owner::kIr);
  TEST_ASSERT_FALSE(a.has(Actions::kSendStop));  // the Uno already stopped
  TEST_ASSERT_TRUE(a.has(Actions::kOwnerChanged));
  TEST_ASSERT_EQUAL(LinkOwner::kNoFd, o.driverFd());
  Actions d;
  TEST_ASSERT_EQUAL(D(Decision::kNackNotOwner), D(o.onDrive(kA, 200, d)));
  o.onUnoOwner(Owner::kNetwork);
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 300, d)));
}

void test_sync_lost_drops_driver_and_rehandshakes() {
  drivingA();
  const Actions a = o.onSynced(false);
  TEST_ASSERT_EQUAL(S(LinkState::kWebHandshake), st());
  TEST_ASSERT_EQUAL(LinkOwner::kNoFd, o.driverFd());
  TEST_ASSERT_TRUE(a.has(Actions::kOwnerChanged));
}

void test_ota_allowed_only_when_nobody_drives() {
  TEST_ASSERT_TRUE(o.otaAllowed());
  readyWithA();
  TEST_ASSERT_TRUE(o.otaAllowed());
  Actions a;
  o.onDrive(kA, 0, a);
  TEST_ASSERT_FALSE(o.otaAllowed());
  o = LinkOwner();
  o.onTcpConnect(a);
  TEST_ASSERT_FALSE(o.otaAllowed());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_first_ws_starts_master);
  RUN_TEST(test_drive_before_sync_is_body_offline);
  RUN_TEST(test_first_drive_takes_ownership);
  RUN_TEST(test_other_phone_gets_not_owner);
  RUN_TEST(test_handback_after_5s_without_drive);
  RUN_TEST(test_release_control);
  RUN_TEST(test_driver_close_with_others_left_stops_and_keeps_master);
  RUN_TEST(test_last_ws_close_stops_and_goes_idle);
  RUN_TEST(test_stop_from_anyone_in_every_state);
  RUN_TEST(test_bench_connect_from_idle_and_ready);
  RUN_TEST(test_bench_refused_while_phone_drives);
  RUN_TEST(test_second_bench_refused);
  RUN_TEST(test_phone_during_passthrough_gets_busy_and_stop_is_injected);
  RUN_TEST(test_bench_close_with_ws_clients_rehandshakes);
  RUN_TEST(test_bench_close_alone_goes_idle);
  RUN_TEST(test_ir_beats_phone);
  RUN_TEST(test_sync_lost_drops_driver_and_rehandshakes);
  RUN_TEST(test_ota_allowed_only_when_nobody_drives);
  return UNITY_END();
}
