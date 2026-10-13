#include <type_traits>
#include <unity.h>

#include "domain/arbitration/arbitrator.h"
#include "domain/estimation/lift_detector.h"
#include "domain/safety/envelope.h"
#include "domain/safety/safety_override.h"
#include "domain/safety/safety_supervisor.h"
#include "domain/selftest/self_test.h"
#include "types/pwm_output.h"

using namespace cq;

void setUp() {}
void tearDown() {}

static_assert(!std::is_constructible<PwmOutput, int16_t, int16_t, bool>::value,
              "only SafetySupervisor may build a moving PwmOutput");

namespace {
const Arbitrator::Config kCfg{300, 3000};
SafetyInputs clearInputs() { return SafetyInputs{false, false, false, true, 0xFFFF, 15, 0, false}; }
Arbitrator syncedArbitrator() {
  Arbitrator a;
  a.setSynced(true);  // HELLO_ACK received
  return a;
}
}  // namespace

void test_ir_owner_ignores_link_loss() {
  Arbitrator a;  // never synced: IR works with no ESP32
  TEST_ASSERT_TRUE(a.onIrInput(0));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), static_cast<int>(a.owner()));
  // ESP32 absent: link never alive
  for (uint32_t t = 0; t < 2000; t += 10) {
    a.onIrInput(t);  // user keeps pressing
    TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(t, false, false, kCfg)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), static_cast<int>(a.owner()));
}

void test_ir_hands_back_after_idle() {
  Arbitrator a;
  a.onIrInput(0);
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(2999, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kIrReleased), static_cast<int>(a.tick(3000, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
}

void test_network_rejected_while_ir_owns() {
  Arbitrator a = syncedArbitrator();
  a.onIrInput(0);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kNotOwner), static_cast<int>(a.requestNetwork(true, 10)));
}

void test_link_loss_stops_network_owner() {
  Arbitrator a = syncedArbitrator();
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(a.requestNetwork(true, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kLinkLostStop), static_cast<int>(a.tick(50, false, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
  Arbitrator c = syncedArbitrator();  // calibration motion was started from the network: stops too
  c.beginCalibration();
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kLinkLostStop), static_cast<int>(c.tick(50, false, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(c.owner()));
}

void test_deadman_only_for_teleop() {
  Arbitrator a = syncedArbitrator();
  a.requestNetwork(true, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(299, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kDeadmanStop), static_cast<int>(a.tick(300, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(310, true, false, kCfg)));  // once
  Arbitrator b = syncedArbitrator();
  b.requestNetwork(false, 0);  // TURN/MOVE primitive running: no stream, no deadman, no idle release
  for (uint32_t t = 0; t < 10000; t += 10) {
    TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(b.tick(t, true, true, kCfg)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNetwork), static_cast<int>(b.owner()));
}

void test_network_motion_busy_until_synced() {
  Arbitrator a;  // boot: no HELLO_ACK yet (LLD §9.4)
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kBusy), static_cast<int>(a.requestNetwork(true, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kBusy), static_cast<int>(a.beginCalibration()));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
  TEST_ASSERT_TRUE(a.onIrInput(10));  // IR is allowed before sync
  a.releaseIr();
  a.setSynced(true);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(a.requestNetwork(true, 20)));
}

void test_network_released_after_1s_idle() {
  Arbitrator a = syncedArbitrator();
  a.requestNetwork(true, 0);
  a.takeOwnerChanged();
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kDeadmanStop), static_cast<int>(a.tick(300, true, false, kCfg)));
  a.tick(999, true, false, kCfg);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNetwork), static_cast<int>(a.owner()));
  a.tick(1000, true, false, kCfg);  // LLD §9.18: 1 s without a network motion command, primitives idle
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
  TEST_ASSERT_TRUE(a.takeOwnerChanged());
  Arbitrator b = syncedArbitrator();
  b.requestNetwork(false, 0);  // a 5 s MOVE keeps the owner
  for (uint32_t t = 0; t < 5000; t += 10) b.tick(t, true, true, kCfg);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNetwork), static_cast<int>(b.owner()));
  b.tick(5000, true, false, kCfg);  // primitive done, last command long ago → released
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(b.owner()));
}

void test_calibration_busy_and_ir_override() {
  Arbitrator a = syncedArbitrator();
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(a.beginCalibration()));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kBusy), static_cast<int>(a.requestNetwork(true, 0)));
  TEST_ASSERT_TRUE(a.onIrInput(5));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), static_cast<int>(a.owner()));
}

void test_envelope_lock_forces_brake() {
  Envelope e;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(e.setBrake(false)));
  TEST_ASSERT_FALSE(e.brakeOn());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(e.set(20, 50, 1)));
  TEST_ASSERT_TRUE(e.brakeOn());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kEnvelopeLocked), static_cast<int>(e.setBrake(false)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand), static_cast<int>(e.set(101, 0, 0)));
  TEST_ASSERT_TRUE(e.active());
}

void test_supervisor_blocks_forward_only() {
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.dist_mm = 100;  // 10 cm < 15 cm
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(0, o.left());
  TEST_ASSERT_TRUE(st.blocked);
  o = s.apply(-150, -150, in, st);
  TEST_ASSERT_EQUAL(-150, o.left());
  o = s.apply(-150, 150, in, st);  // spin in place is allowed
  TEST_ASSERT_EQUAL(150, o.right());
  in.brake_on = false;
  o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(150, o.left());
  TEST_ASSERT_FALSE(st.blocked);
}

void test_fault_range_does_not_block() {
  // 0xFFFE = ultrasonic fault: the obstacle brake is unavailable (no phantom obstacle). The degraded
  // 50 % cap is applied to the targets by the TargetLimiter (Task 8), not here.
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.dist_mm = 0xFFFE;
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(150, o.left());
  TEST_ASSERT_EQUAL(150, o.right());
  TEST_ASSERT_FALSE(st.blocked);
}

void test_supervisor_cuts_stalled_track() {
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.stall_mask = 0x01;  // left stalled (LLD §9.6): only that track is cut
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(0, o.left());
  TEST_ASSERT_EQUAL(150, o.right());
  TEST_ASSERT_TRUE(o.enabled());
  in.stall_mask = 0x02;
  o = s.apply(-150, 150, in, st);
  TEST_ASSERT_EQUAL(-150, o.left());
  TEST_ASSERT_EQUAL(0, o.right());
}

void test_supervisor_lift_coasts_tilt_halts_clamps() {
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.lifted = true;
  PwmOutput o = s.apply(200, 200, in, st);
  TEST_ASSERT_FALSE(o.enabled());
  TEST_ASSERT_TRUE(st.coast);
  in = clearInputs();
  in.tilted = true;
  o = s.apply(200, 200, in, st);
  TEST_ASSERT_EQUAL(0, o.right());
  TEST_ASSERT_TRUE(st.halted);
  in = clearInputs();
  o = s.apply(400, -400, in, st);
  TEST_ASSERT_EQUAL(255, o.left());
  TEST_ASSERT_EQUAL(-255, o.right());
}

// ---- Safety Override (LLD §10): human only, bounded, relaxes the obstacle brake and the tilt limit only ----
namespace {
SafetyOverride::Gate okGate() { return SafetyOverride::Gate{true, false, false, proto::Owner::kNetwork}; }
const SafetyOverride::Config kOvCfg{60, 300};
void assertEvent(SafetyOverride& o, bool on, proto::OverrideReason why) {
  bool ev_on = !on;
  proto::OverrideReason ev_why = proto::OverrideReason::kNetwork;
  TEST_ASSERT_TRUE(o.takeEvent(ev_on, ev_why));
  TEST_ASSERT_EQUAL(on, ev_on);
  TEST_ASSERT_EQUAL(static_cast<int>(why), static_cast<int>(ev_why));
}
int st(proto::AckStatus s) { return static_cast<int>(s); }
}  // namespace

void test_override_network_gate() {
  SafetyOverride o;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kRoleForbidden), st(o.requestNetwork(true, proto::Source::kAi, okGate(), 0)));
  SafetyOverride::Gate g = okGate();
  g.synced = false;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kBusy), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  g = okGate();
  g.child_play = true;  // child play keeps all safety
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kEnvelopeLocked), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  g = okGate();
  g.owner = proto::Owner::kIr;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kNotOwner), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  g = okGate();
  g.fault_latched = true;  // never un-halts a tilted tank
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kBlocked), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  bool on = false;
  proto::OverrideReason why{};
  TEST_ASSERT_FALSE(o.takeEvent(on, why));  // refusals post nothing
  TEST_ASSERT_FALSE(o.active());
  g = okGate();
  g.owner = proto::Owner::kNone;  // nobody drives yet: a human may still enable it
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  TEST_ASSERT_TRUE(o.active());
  assertEvent(o, true, proto::OverrideReason::kNetwork);
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(o.requestNetwork(true, proto::Source::kHuman, okGate(), 5)));
  TEST_ASSERT_FALSE(o.takeEvent(on, why));  // repeat: no second event
  // off is accepted from any source, the AI included (it only tightens)
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(o.requestNetwork(false, proto::Source::kAi, okGate(), 10)));
  TEST_ASSERT_FALSE(o.active());
  assertEvent(o, false, proto::OverrideReason::kNetwork);
}

void test_override_ir_long_press_toggles() {
  SafetyOverride o;
  SafetyOverride::Gate g{false, false, false, proto::Owner::kIr};  // IR works without the ESP32
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(o.requestIr(g, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(SafetyOverride::Binder::kIr), static_cast<int>(o.binder()));
  assertEvent(o, true, proto::OverrideReason::kIr);
  o.onLinkLost();  // the IR side does not need the link
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(o.requestIr(g, 100)));  // second hold: off
  TEST_ASSERT_FALSE(o.active());
  assertEvent(o, false, proto::OverrideReason::kIr);
  g.child_play = true;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kEnvelopeLocked), st(o.requestIr(g, 200)));
  g.child_play = false;
  g.fault_latched = true;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kBlocked), st(o.requestIr(g, 200)));
  TEST_ASSERT_FALSE(o.active());
}

void test_override_expires_idle_and_max() {
  SafetyOverride o;
  bool on = false;
  proto::OverrideReason why{};
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 1000);
  o.takeEvent(on, why);
  o.onNetworkMotion(proto::Source::kHuman, true, 30000);  // driving refreshes the idle timer
  o.tick(89999, kOvCfg);
  TEST_ASSERT_TRUE(o.active());
  o.onNetworkMotion(proto::Source::kHuman, false, 60000);  // DRIVE 0,0 is not driving
  o.tick(90000, kOvCfg);  // 60 s after the last driving command
  TEST_ASSERT_FALSE(o.active());
  assertEvent(o, false, proto::OverrideReason::kIdle);
  // 5 min hard limit even while driving all the time
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  for (uint32_t t = 0; t < 300000; t += 1000) {
    o.onNetworkMotion(proto::Source::kHuman, true, t);
    o.tick(t, kOvCfg);
  }
  TEST_ASSERT_TRUE(o.active());
  o.tick(300000, kOvCfg);
  TEST_ASSERT_FALSE(o.active());
  assertEvent(o, false, proto::OverrideReason::kMaxTime);
}

void test_override_ends_on_stop_owner_change_link_loss() {
  SafetyOverride o;
  bool on = false;
  proto::OverrideReason why{};
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  o.end(proto::OverrideReason::kStop);  // any STOP (network or IR OK)
  assertEvent(o, false, proto::OverrideReason::kStop);
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  o.onOwner(proto::Owner::kNone);  // idle release alone keeps it (the 60 s timer ends it)
  TEST_ASSERT_TRUE(o.active());
  o.onOwner(proto::Owner::kIr);  // IR took over
  assertEvent(o, false, proto::OverrideReason::kOwnerChange);
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  o.onNetworkMotion(proto::Source::kAi, true, 10);  // the AI never drives with Override on
  assertEvent(o, false, proto::OverrideReason::kOwnerChange);
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  o.onOwnerHint(proto::OwnerHintValue::kNone);  // human idle on the ESP32: kept
  TEST_ASSERT_TRUE(o.active());
  o.onOwnerHint(proto::OwnerHintValue::kAi);  // handed to the AI
  assertEvent(o, false, proto::OverrideReason::kOwnerChange);
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  o.onLinkLost();
  assertEvent(o, false, proto::OverrideReason::kLinkLost);
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  o.takeEvent(on, why);
  o.end(proto::OverrideReason::kEnvelope);  // ENVELOPE frame applied
  assertEvent(o, false, proto::OverrideReason::kEnvelope);
}

void test_override_relaxes_brake_only() {
  SafetyOverride o;
  o.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  SafetySupervisor s;
  SafetyStatus sst{};
  SafetyInputs in = clearInputs();
  in.dist_mm = 100;  // 10 cm: would block
  in.brake_on = o.brakeOn(true);
  PwmOutput out = s.apply(150, 150, in, sst);
  TEST_ASSERT_EQUAL(150, out.left());
  TEST_ASSERT_FALSE(sst.blocked);
  in.lifted = true;  // lift still coasts
  out = s.apply(150, 150, in, sst);
  TEST_ASSERT_FALSE(out.enabled());
  in.lifted = false;
  in.batt_critical = true;  // battery critical still halts
  out = s.apply(150, 150, in, sst);
  TEST_ASSERT_EQUAL(0, out.left());
  TEST_ASSERT_TRUE(sst.halted);
  in.batt_critical = false;
  in.stall_mask = 0x02;  // stall still cuts the track
  out = s.apply(150, 150, in, sst);
  TEST_ASSERT_EQUAL(0, out.right());
  // deadman still fires (the Arbitrator never consults the override)
  Arbitrator a = syncedArbitrator();
  a.requestNetwork(true, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kDeadmanStop), static_cast<int>(a.tick(300, true, false, kCfg)));
  o.end(proto::OverrideReason::kStop);
  TEST_ASSERT_TRUE(o.brakeOn(true));  // the BRAKE setting applies again
  TEST_ASSERT_FALSE(o.brakeOn(false));
}

void test_override_allowed_with_adult_envelope() {
  // Owner decision 4 (2026-10-12): only the child-play envelope blocks Override.
  Envelope adult;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(adult.set(50, 40, 1)));  // custom caps + brake lock
  TEST_ASSERT_FALSE(adult.childPlay());
  TEST_ASSERT_EQUAL_HEX8(0x01, adult.flags());
  SafetyOverride::Gate g = okGate();
  g.child_play = adult.childPlay();
  SafetyOverride o;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  TEST_ASSERT_TRUE(o.active());
  // the adult caps stay in force (TargetLimiter reads them); only the brake lock is relaxed
  TEST_ASSERT_EQUAL(50, adult.speedCap());
  TEST_ASSERT_EQUAL(40, adult.accelCap());
  TEST_ASSERT_FALSE(o.brakeOn(adult.brakeOn()));
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kEnvelopeLocked), st(adult.setBrake(false)));  // BRAKE off still refused
  // child play: bit 1 (implies the lock) refuses Override; unknown bits are invalid
  Envelope child;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kOk), st(child.set(20, 30, 2)));
  TEST_ASSERT_TRUE(child.childPlay());
  TEST_ASSERT_TRUE(child.lockBrake());
  TEST_ASSERT_EQUAL_HEX8(0x03, child.flags());
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kInvalidCommand), st(child.set(20, 30, 4)));
  SafetyOverride c;
  g.child_play = child.childPlay();
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kEnvelopeLocked), st(c.requestNetwork(true, proto::Source::kHuman, g, 0)));
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kEnvelopeLocked), st(c.requestIr(g, 0)));
}

void test_override_tilt_65_with_hold() {
  SafetyOverride o;
  TEST_ASSERT_EQUAL(45, o.tiltLimit(45, 65));
  o.requestIr(SafetyOverride::Gate{false, false, false, proto::Owner::kIr}, 0);
  TEST_ASSERT_EQUAL(65, o.tiltLimit(45, 65));
  TEST_ASSERT_EQUAL(50, o.tiltLimit(50, 45));  // never below the normal limit
  LiftDetector l;
  for (uint32_t t = 0; t < 2000; t += 5)  // 55° is fine under Override
    TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                      static_cast<int>(l.update(1.0f, 55.0f, 0, o.tiltLimit(45, 65), t)));
  for (uint32_t t = 2000; t < 2300; t += 5)  // past 65° still needs the 300 ms hold
    TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                      static_cast<int>(l.update(1.0f, 66.0f, 0, o.tiltLimit(45, 65), t)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kTilted),
                    static_cast<int>(l.update(1.0f, 66.0f, 0, o.tiltLimit(45, 65), 2300)));
  // after exit the normal 45° limit applies again (with its hold)
  LiftDetector m;
  o.end(proto::OverrideReason::kIr);
  for (uint32_t t = 0; t < 300; t += 5) m.update(1.0f, 55.0f, 0, o.tiltLimit(45, 65), t);
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kTilted),
                    static_cast<int>(m.update(1.0f, 55.0f, 0, o.tiltLimit(45, 65), 300)));
}

// ---- Battery-only lock auto-recovery (LLD §10.9, owner 2026-10-12) ----
void test_battery_only_lock_clears_after_1s_valid() {
  SelfTest t;
  t.begin();
  t.report(SelfTest::Check::kImu, true);
  t.report(SelfTest::Check::kUltrasonic, false);  // a warning stays reported but never holds the lock
  t.report(SelfTest::Check::kBattery, SelfTest::batteryPlausible(0));  // USB only
  t.complete();
  TEST_ASSERT_TRUE(t.locked());
  TEST_ASSERT_EQUAL(1, t.runs());
  TEST_ASSERT_FALSE(t.tickBattery(false, 0));
  TEST_ASSERT_FALSE(t.tickBattery(true, 100));  // battery on: the 1 s starts
  TEST_ASSERT_FALSE(t.tickBattery(true, 1099));  // 999 ms
  TEST_ASSERT_FALSE(t.tickBattery(false, 1200));  // a glitch restarts it
  TEST_ASSERT_FALSE(t.tickBattery(true, 1300));
  TEST_ASSERT_FALSE(t.tickBattery(true, 2299));  // 999 ms again
  TEST_ASSERT_TRUE(t.locked());
  TEST_ASSERT_TRUE(t.tickBattery(true, 2300));  // 1000 ms continuously valid → cleared
  TEST_ASSERT_FALSE(t.locked());
  TEST_ASSERT_EQUAL_HEX8(0x02, t.failMask());  // only the ultrasonic warning is left
  TEST_ASSERT_EQUAL(2, t.runs());  // the UI replays SUCCESS
  TEST_ASSERT_FALSE(t.tickBattery(true, 2400));  // nothing more to do once unlocked
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.selftest_locked = t.locked();
  TEST_ASSERT_EQUAL(150, s.apply(150, 150, in, st).left());
  // clock wrap: 1 s counted with unsigned subtraction
  SelfTest w;
  w.begin();
  w.report(SelfTest::Check::kBattery, false);
  w.complete();
  TEST_ASSERT_FALSE(w.tickBattery(true, 0xFFFFFF00u));
  TEST_ASSERT_TRUE(w.tickBattery(true, 0xFFFFFF00u + 1000u));
  TEST_ASSERT_FALSE(w.locked());
}

void test_imu_lock_not_auto_cleared() {
  SelfTest t;  // IMU alone
  t.begin();
  t.report(SelfTest::Check::kImu, false);
  t.report(SelfTest::Check::kBattery, true);
  t.complete();
  for (uint32_t ms = 0; ms <= 5000; ms += 100) TEST_ASSERT_FALSE(t.tickBattery(true, ms));
  TEST_ASSERT_TRUE(t.locked());
  SelfTest b;  // IMU + battery: a good battery does not clear either failure
  b.begin();
  b.report(SelfTest::Check::kImu, false);
  b.report(SelfTest::Check::kBattery, false);
  b.complete();
  for (uint32_t ms = 0; ms <= 5000; ms += 100) TEST_ASSERT_FALSE(b.tickBattery(true, ms));
  TEST_ASSERT_TRUE(b.locked());
  TEST_ASSERT_EQUAL_HEX8(0x05, b.failMask());
  b.begin();  // only an explicit re-run with both fixed unlocks
  b.report(SelfTest::Check::kImu, true);
  b.report(SelfTest::Check::kBattery, true);
  b.complete();
  TEST_ASSERT_FALSE(b.locked());
  // a battery-only lock is not auto-cleared while a re-run is in progress (the run's verdict decides)
  SelfTest r;
  r.begin();
  r.report(SelfTest::Check::kBattery, false);
  r.complete();
  r.begin();
  TEST_ASSERT_FALSE(r.tickBattery(true, 0));
  TEST_ASSERT_FALSE(r.tickBattery(true, 1500));
  TEST_ASSERT_TRUE(r.locked());
}

// ---- Critical self-test lock (LLD §10.9, owner decision 6) ----
void test_critical_selftest_locks_motion() {
  SelfTest t;
  t.begin();
  t.report(SelfTest::Check::kImu, false);  // MPU6050 missing
  t.report(SelfTest::Check::kBattery, true);
  t.complete();
  TEST_ASSERT_TRUE(t.locked());
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.selftest_locked = t.locked();
  // network teleop, an IR key and a calibration script all reach the motors through apply()
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(0, o.left());
  TEST_ASSERT_EQUAL(0, o.right());
  TEST_ASSERT_TRUE(st.locked);
  TEST_ASSERT_TRUE(st.halted);  // running primitives are cancelled like on any halt
  o = s.apply(-200, 120, in, st);
  TEST_ASSERT_EQUAL(0, o.left());
  TEST_ASSERT_EQUAL(0, o.right());
  // battery read implausible (USB only / open divider) is critical too; ultrasonic and settings are warnings
  SelfTest b;
  b.begin();
  b.report(SelfTest::Check::kBattery, SelfTest::batteryPlausible(0));
  b.complete();
  TEST_ASSERT_TRUE(b.locked());
  TEST_ASSERT_TRUE(SelfTest::batteryPlausible(7400));
  SelfTest w;
  w.begin();
  w.report(SelfTest::Check::kUltrasonic, false);
  w.report(SelfTest::Check::kSettings, false);
  w.complete();
  TEST_ASSERT_EQUAL_HEX8(0x0A, w.failMask());
  TEST_ASSERT_FALSE(w.locked());
}

void test_selftest_rerun_pass_unlocks() {
  SelfTest t;
  t.begin();
  t.report(SelfTest::Check::kImu, false);
  t.complete();
  TEST_ASSERT_TRUE(t.locked());
  TEST_ASSERT_EQUAL(1, t.runs());
  t.begin();  // SELF_TEST re-run: the lock holds until the verdict
  TEST_ASSERT_TRUE(t.running());
  TEST_ASSERT_TRUE(t.locked());
  t.report(SelfTest::Check::kImu, false);  // still failing → still locked
  t.complete();
  TEST_ASSERT_TRUE(t.locked());
  t.begin();
  t.report(SelfTest::Check::kImu, true);  // IMU back (1 s re-init) → pass unlocks
  t.report(SelfTest::Check::kBattery, true);
  t.report(SelfTest::Check::kUltrasonic, false);  // a warning does not keep the lock
  t.complete();
  TEST_ASSERT_FALSE(t.locked());
  TEST_ASSERT_FALSE(t.running());
  TEST_ASSERT_EQUAL(3, t.runs());
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.selftest_locked = t.locked();
  TEST_ASSERT_EQUAL(150, s.apply(150, 150, in, st).left());
  TEST_ASSERT_FALSE(st.locked);
}

void test_override_cannot_unlock_selftest() {
  SelfTest t;
  t.begin();
  t.report(SelfTest::Check::kImu, false);
  t.complete();
  SafetyOverride::Gate g = okGate();
  g.fault_latched = t.locked();  // app: overrideGate() folds the lock into fault_latched
  SafetyOverride o;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kBlocked), st(o.requestNetwork(true, proto::Source::kHuman, g, 0)));
  g.owner = proto::Owner::kIr;
  TEST_ASSERT_EQUAL(st(proto::AckStatus::kBlocked), st(o.requestIr(g, 0)));
  TEST_ASSERT_FALSE(o.active());
  // even with Override already on (entered before a failing re-run), the supervisor keeps every track still
  SafetyOverride on;
  on.requestNetwork(true, proto::Source::kHuman, okGate(), 0);
  TEST_ASSERT_TRUE(on.active());
  SafetySupervisor s;
  SafetyStatus sst{};
  SafetyInputs in = clearInputs();
  in.brake_on = on.brakeOn(true);
  in.selftest_locked = t.locked();
  const PwmOutput out = s.apply(200, 200, in, sst);
  TEST_ASSERT_EQUAL(0, out.left());
  TEST_ASSERT_EQUAL(0, out.right());
  TEST_ASSERT_TRUE(sst.locked);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ir_owner_ignores_link_loss);
  RUN_TEST(test_ir_hands_back_after_idle);
  RUN_TEST(test_network_rejected_while_ir_owns);
  RUN_TEST(test_link_loss_stops_network_owner);
  RUN_TEST(test_deadman_only_for_teleop);
  RUN_TEST(test_network_motion_busy_until_synced);
  RUN_TEST(test_network_released_after_1s_idle);
  RUN_TEST(test_calibration_busy_and_ir_override);
  RUN_TEST(test_envelope_lock_forces_brake);
  RUN_TEST(test_supervisor_blocks_forward_only);
  RUN_TEST(test_fault_range_does_not_block);
  RUN_TEST(test_supervisor_cuts_stalled_track);
  RUN_TEST(test_supervisor_lift_coasts_tilt_halts_clamps);
  RUN_TEST(test_override_network_gate);
  RUN_TEST(test_override_ir_long_press_toggles);
  RUN_TEST(test_override_expires_idle_and_max);
  RUN_TEST(test_override_ends_on_stop_owner_change_link_loss);
  RUN_TEST(test_override_relaxes_brake_only);
  RUN_TEST(test_override_allowed_with_adult_envelope);
  RUN_TEST(test_override_tilt_65_with_hold);
  RUN_TEST(test_critical_selftest_locks_motion);
  RUN_TEST(test_selftest_rerun_pass_unlocks);
  RUN_TEST(test_override_cannot_unlock_selftest);
  RUN_TEST(test_battery_only_lock_clears_after_1s_valid);
  RUN_TEST(test_imu_lock_not_auto_cleared);
  return UNITY_END();
}
