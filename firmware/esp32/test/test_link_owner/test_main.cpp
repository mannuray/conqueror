// LinkOwner: every transition of the bridge LLD R2.7 state diagram, R14: one master, always on.
#include <unity.h>

#include "core/link_owner.h"

using cq::core::Actions;
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
  o.begin();
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

void test_begin_starts_the_master_once() {  // R14: at boot, with no phone
  TEST_ASSERT_EQUAL(S(LinkState::kIdle), st());
  const Actions a = o.begin();
  TEST_ASSERT_TRUE(a.has(Actions::kStartMaster));
  TEST_ASSERT_EQUAL(S(LinkState::kWebHandshake), st());
  TEST_ASSERT_FALSE(o.begin().has(Actions::kStartMaster));
}

void test_a_phone_opening_starts_nothing() {  // R14: phones never change the link
  TEST_ASSERT_EQUAL(0, o.onWsOpen(kA).bits);
  TEST_ASSERT_EQUAL(S(LinkState::kIdle), st());
  o.begin();
  TEST_ASSERT_EQUAL(0, o.onWsOpen(kB).bits);
  TEST_ASSERT_EQUAL(2, o.wsCount());
}

void test_drive_before_sync_is_body_offline() {
  o.begin();
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
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
}

void test_last_ws_close_of_the_driver_stops_and_keeps_master() {  // R14
  drivingA();
  const Actions a = o.onWsClose(kA);
  TEST_ASSERT_TRUE(a.has(Actions::kSendStop));
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
  TEST_ASSERT_EQUAL(0, o.wsCount());
}

void test_last_ws_close_of_a_watcher_changes_nothing() {  // R14: no STOP, no master change
  readyWithA();
  TEST_ASSERT_EQUAL(0, o.onWsClose(kA).bits);
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
}

void test_stop_from_anyone_in_every_state() {
  TEST_ASSERT_TRUE(o.onStop(kB).has(Actions::kSendStop));  // Idle
  drivingA();
  o.onWsOpen(kB);
  TEST_ASSERT_TRUE(o.onStop(kB).has(Actions::kSendStop));  // not the driver: still accepted
  TEST_ASSERT_EQUAL(kA, o.driverFd());                      // STOP does not change the owner
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
}

// Plan H review I1: the motion service keeps an existing owner alive but never takes ownership.
void test_keep_driving_never_takes_ownership() {
  readyWithA();
  TEST_ASSERT_EQUAL(D(Decision::kNackNotOwner), D(o.keepDriving(kA, 100)));
  TEST_ASSERT_EQUAL(S(LinkState::kWebReady), st());
  TEST_ASSERT_EQUAL(LinkOwner::kNoFd, o.driverFd());
  drivingA(100);
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.keepDriving(kA, 4000)));
  TEST_ASSERT_EQUAL(0, o.tick(5100).bits);  // refreshed at 4000: no handback yet
  o.onWsOpen(kB);
  TEST_ASSERT_EQUAL(D(Decision::kNackNotOwner), D(o.keepDriving(kB, 4100)));
  o.onUnoOwner(Owner::kIr);
  TEST_ASSERT_EQUAL(D(Decision::kNackNotOwner), D(o.keepDriving(kA, 4200)));
}


// Safety fix (design-notes §72): a STOP from a device that is not driving must hold - the driver's next
// DRIVE (sent 20 times a second while its stick is held) would otherwise restart the tank at once.
void test_stop_from_another_device_holds_until_the_stick_is_released() {
  drivingA();
  o.onWsOpen(kB);
  o.onStop(kB);
  Actions a;
  TEST_ASSERT_EQUAL(D(Decision::kNackStopped), D(o.onDrive(kA, 200, a, true)));   // stick still held: refused
  TEST_ASSERT_EQUAL(D(Decision::kNackStopped), D(o.onDrive(kA, 250, a, true)));
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 300, a, false)));         // released (0/0): passes ...
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 350, a, true)));          // ... and drives again
  TEST_ASSERT_EQUAL(kA, o.driverFd());
}

void test_the_drivers_own_stop_does_not_hold() {
  drivingA();
  o.onStop(kA);  // the drive page's STOP / stick release
  Actions a;
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 200, a, true)));
}

void test_a_new_primitive_or_the_drivers_stop_releases_the_hold() {
  drivingA();
  o.onWsOpen(kB);
  o.onStop(kB);
  Actions a;
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 200, a)));  // turn / move: a deliberate new command
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 250, a, true)));
  o.onStop(kB);
  o.onStop(kA);  // the driver lets go (its page sends stop)
  TEST_ASSERT_EQUAL(D(Decision::kSend), D(o.onDrive(kA, 300, a, true)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_begin_starts_the_master_once);
  RUN_TEST(test_a_phone_opening_starts_nothing);
  RUN_TEST(test_drive_before_sync_is_body_offline);
  RUN_TEST(test_first_drive_takes_ownership);
  RUN_TEST(test_other_phone_gets_not_owner);
  RUN_TEST(test_handback_after_5s_without_drive);
  RUN_TEST(test_release_control);
  RUN_TEST(test_keep_driving_never_takes_ownership);
  RUN_TEST(test_driver_close_with_others_left_stops_and_keeps_master);
  RUN_TEST(test_last_ws_close_of_the_driver_stops_and_keeps_master);
  RUN_TEST(test_last_ws_close_of_a_watcher_changes_nothing);
  RUN_TEST(test_stop_from_anyone_in_every_state);
  RUN_TEST(test_ir_beats_phone);
  RUN_TEST(test_sync_lost_drops_driver_and_rehandshakes);
  RUN_TEST(test_ota_allowed_only_when_nobody_drives);
  RUN_TEST(test_stop_from_another_device_holds_until_the_stick_is_released);
  RUN_TEST(test_the_drivers_own_stop_does_not_hold);
  RUN_TEST(test_a_new_primitive_or_the_drivers_stop_releases_the_hold);
  return UNITY_END();
}
