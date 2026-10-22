// Plan N3: LogoController with fake hooks - run and its refusals, waiting for the Uno's ACK, SAY's pause, PLAY in
// chunks, LOOK angles, every ending stops the tank and gives the drive back, a takeover, step mode, the pose.
#include <string.h>
#include <unity.h>

#include <vector>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/logo_controller.h"
#include "fakes/fake_ports.h"
#include "logo/check_job.h"

using namespace cq;
using app::LogoController;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::EventLog logbook;
core::FrameTx tx;
core::BodyLink* link = nullptr;
app::TelemetryPublisher* tel = nullptr;
logo::Program g_running, g_checked;
logo::Vm g_vm;
logo::CheckSpace g_space;
LogoController* lc = nullptr;

core::Decision g_claim = core::Decision::kSend;
core::Decision g_keep = core::Decision::kSend;
int g_keeps = 0;
bool g_stop_owned = false;
bool g_ota = false;
LogoController::Took g_took = LogoController::Took::kOk;
bool g_motion_ok = true;
core::BridgeReason g_motion_why = core::BridgeReason::kBodyOffline;
bool g_queue_ok = true;
std::vector<logo::Action> g_motions;
std::vector<core::AckedOp> g_ops;
int g_stops = 0, g_releases = 0;
int g_go_keeps = 0;  // Tank Logo LLD §22: keep_go calls
uint32_t g_now = 1000;

void unoSends(const uint8_t* f, size_t n, uint32_t now) { link->onUartBytes(f, n, now); }

void synced() {
  link->startMaster(0);
  uint8_t f[64];
  size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 1, 1, 0x1UL}, 1, f);
  unoSends(f, n, 10);
  proto::TelemFastMsg t{};
  t.dist_mm = 65535;
  n = cqtest::frame(t, 2, f);
  unoSends(f, n, 20);
}

void hooks() {
  LogoController::Hooks h;
  h.claim = [](int, uint32_t) { return g_claim; };
  h.keep = [](int, uint32_t) {
    ++g_keeps;
    return g_keep;
  };
  h.ota = [] { return g_ota; };
  h.take = [](uint32_t, logo::Program& p, char* name, size_t cap) {
    if (g_took == LogoController::Took::kOk) {
      p = g_checked;
      snprintf(name, cap, "square");
    }
    return g_took;
  };
  h.start_motion = [](int, const logo::Action& a, uint32_t, core::BridgeReason& why) {
    g_motions.push_back(a);
    why = g_motion_why;
    return g_motion_ok;
  };
  h.stop_tank = [](bool owns, uint32_t) {
    ++g_stops;
    g_stop_owned = owns;
  };
  h.release = [](int, uint32_t) { ++g_releases; };
  h.uno_request = [](const core::AckedOp& op) {
    if (g_queue_ok) g_ops.push_back(op);
    return g_queue_ok;
  };
  h.cpm = [] { return static_cast<uint16_t>(7514); };
  h.keep_go = [](uint32_t) { ++g_go_keeps; };
  lc->setHooks(h);
}

// Checks the program into the box and runs it as phone 3.
void run(const char* src, bool step = false) {
  TEST_ASSERT_TRUE_MESSAGE(logo::checkProgram(src, static_cast<uint16_t>(strlen(src)), logo::Caps(), g_space), src);
  g_checked = g_space.program;
  lc->onRun(3, 5, 1, step, g_now);
}

void ticks(int n) {
  for (int i = 0; i < n; ++i) lc->tick(g_now += 2);
}
void done() {  // the motion the program waits for has ended normally
  core::PrimitiveEnd e;
  e.ok = true;
  lc->onPrimitiveEnd(e, g_now);
}
bool sent(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  ws.add(4, 0);
  logbook = core::EventLog();
  tx = core::FrameTx();
  delete lc;
  delete tel;
  delete link;
  link = new core::BodyLink(tx);
  tel = new app::TelemetryPublisher(out, *link);
  lc = new LogoController(out, logbook, *link, *tel, g_running, g_vm);
  synced();
  hooks();
  g_claim = core::Decision::kSend;
  g_keep = core::Decision::kSend;
  g_keeps = 0;
  g_stop_owned = false;
  g_ota = false;
  g_took = LogoController::Took::kOk;
  g_motion_ok = true;
  g_queue_ok = true;
  g_motions.clear();
  g_ops.clear();
  g_stops = g_releases = 0;
  g_go_keeps = 0;
}
void tearDown() {}

void test_run_starts_and_carries_out_moves() {
  run("FD 50 RT 90");
  TEST_ASSERT_TRUE(sent(3, "\"t\":\"ack\",\"id\":5"));
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_started\",\"name\":\"square\",\"by\":\"phone-"));
  TEST_ASSERT_TRUE(lc->running());
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kMove, g_motions[0].kind);
  TEST_ASSERT_EQUAL(50, g_motions[0].dist_cm);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_line\",\"line\":1"));
  ticks(5);
  TEST_ASSERT_EQUAL(1, g_motions.size());  // waits for the move's end
  done();
  ticks(1);
  TEST_ASSERT_EQUAL(2, g_motions.size());
  TEST_ASSERT_EQUAL(-9000, g_motions[1].delta_cdeg);
  done();
  ticks(1);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_ended\",\"reason\":\"finished\""));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_TRUE(g_stop_owned);  // still its drive: the STOP goes
  TEST_ASSERT_EQUAL(1, g_releases);
}

void test_run_refusals() {
  g_took = LogoController::Took::kCheckAgain;
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "CHECK_AGAIN"));
  g_took = LogoController::Took::kBusy;
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "BUSY"));
  g_took = LogoController::Took::kOk;
  g_claim = core::Decision::kNackNotOwner;
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "NOT_OWNER"));
  TEST_ASSERT_FALSE(lc->running());
  g_claim = core::Decision::kSend;
  g_ota = true;
  fakes::reset();
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "BUSY"));
  g_ota = false;
  run("FOREVER [ WAIT 1 ]");
  fakes::reset();
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "BUSY"));  // one program at a time
}

void test_light_waits_for_the_uno_s_ack() {
  run("LED GREEN FD 10");
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
  TEST_ASSERT_EQUAL(core::AckedOp::kLed, g_ops[0].kind);
  TEST_ASSERT_EQUAL(core::kLogoClient, g_ops[0].fd);
  TEST_ASSERT_EQUAL(255, g_ops[0].g);
  ticks(20);
  TEST_ASSERT_EQUAL(0, g_motions.size());  // still waiting
  lc->onUnoDone(0, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_motions.size());
}

void test_no_answer_ends_with_7034_a_refusal_goes_on() {
  run("SOUND HORN BEEP FD 10");
  ticks(1);
  TEST_ASSERT_EQUAL(24, g_ops[0].a);
  lc->onUnoDone(static_cast<uint8_t>(proto::AckStatus::kBusy), g_now);  // refused: logged, goes on
  ticks(1);
  TEST_ASSERT_EQUAL(2, g_ops.size());
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::SoundId::kBlip), g_ops[1].a);  // BEEP
  lc->onUnoDone(app::UnoChannel::kLogoTimeout, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7034"));
  TEST_ASSERT_EQUAL(1, g_stops);
}

void test_a_full_queue_is_tried_again() {
  g_queue_ok = false;
  run("BEEP");
  ticks(3);
  TEST_ASSERT_EQUAL(0, g_ops.size());
  g_queue_ok = true;
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
}

void test_say_then_a_pause() {
  run("SAY [ hello there ] FD 10");
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_say\",\"text\":\"hello there\""));
  ticks(50);  // 100 ms
  TEST_ASSERT_EQUAL(0, g_motions.size());
  ticks(60);  // past 200 ms
  TEST_ASSERT_EQUAL(1, g_motions.size());
}

void test_play_goes_in_chunks_of_ten_after_each_other() {
  run("PLAY [ C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5 ]");  // 12 notes of 250 ms
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
  TEST_ASSERT_EQUAL(core::AckedOp::kTone, g_ops[0].kind);
  TEST_ASSERT_EQUAL(10, g_ops[0].n_notes);
  lc->onUnoDone(0, g_now);
  ticks(100);  // 200 ms: the first 10 notes play for 2.5 s
  TEST_ASSERT_EQUAL(1, g_ops.size());
  ticks(1200);  // 2.4 s more
  TEST_ASSERT_EQUAL(2, g_ops.size());
  TEST_ASSERT_EQUAL(2, g_ops[1].n_notes);
  TEST_ASSERT_EQUAL(784, g_ops[1].hz[1]);  // G5
}

void test_look_angles_and_home() {
  run("LOOK [ 30 -10 ] LOOK HOME");
  ticks(1);
  TEST_ASSERT_EQUAL(core::AckedOp::kCamSet, g_ops[0].kind);
  TEST_ASSERT_EQUAL(600, g_ops[0].pan_dd);   // right 30° = the smaller servo angle
  TEST_ASSERT_EQUAL(1000, g_ops[0].tilt_dd);  // down 10°
  lc->onUnoDone(0, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(core::AckedOp::kCamHome, g_ops[1].kind);
}

void test_stop_while_waiting_ends_once() {
  run("WAIT 10 FD 10");
  ticks(1);
  lc->onMotionEvent(core::MotionEvent::kStop, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7040"));
  lc->onMotionEvent(core::MotionEvent::kOwnerClosed, g_now);
  lc->onPrimitiveEnd(core::PrimitiveEnd(), g_now);
  TEST_ASSERT_EQUAL(1, fakes::countTo(4, "logo_ended"));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_EQUAL(1, g_releases);
}

void test_another_device_taking_the_drive_ends_it_without_stopping_them() {
  run("FOREVER [ WAIT 1 ]");
  ticks(2);
  g_keep = core::Decision::kNackNotOwner;  // a phone or the IR remote took over
  ticks(1);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7045"));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_FALSE(g_stop_owned);  // bridge LLD R4.6: no STOP for the new driver
}

void test_a_waiting_program_keeps_the_drive_every_tick() {
  run("WAIT 10");
  ticks(3000);  // 6 s: longer than the 5 s handback
  TEST_ASSERT_TRUE(lc->running());
  TEST_ASSERT_TRUE(g_keeps >= 3000);
}

void test_the_body_gone_ends_it_with_7034() {
  run("FOREVER [ WAIT 1 ]");
  g_keep = core::Decision::kNackBodyOffline;
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7034"));
}

void test_logo_stop_from_any_page_and_not_running() {
  run("FOREVER [ WAIT 1 ]");
  lc->onStop(4, 9, g_now);
  TEST_ASSERT_TRUE(sent(4, "\"t\":\"ack\",\"id\":9"));
  TEST_ASSERT_TRUE(sent(3, "\"reason\":7040"));
  lc->onStop(4, 10, g_now);
  TEST_ASSERT_TRUE(sent(4, "NOT_RUNNING"));
  lc->onStep(4, 11);
  TEST_ASSERT_EQUAL(2, fakes::countTo(4, "NOT_RUNNING"));
}

void test_a_motion_that_cannot_start_ends_it() {
  g_motion_ok = false;
  g_motion_why = core::BridgeReason::kUnavailable;
  run("FD 10");
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7021"));
  TEST_ASSERT_FALSE(lc->running());
}

void test_battery_and_tilt_alarms() {
  run("FOREVER [ WAIT 1 ]");
  lc->onUnoAlarm(logo::RunEvent::kBatteryLow, g_now);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7042"));
  run("FOREVER [ WAIT 1 ]");
  lc->onUnoAlarm(logo::RunEvent::kTilt, g_now);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7043"));
}

void test_step_mode() {
  run("BEEP\nBEEP", true);
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_line\",\"line\":1"));
  ticks(5);
  TEST_ASSERT_EQUAL(0, g_ops.size());  // paused before line 1
  lc->onStep(3, 12);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
}

void test_pose_ten_times_a_second() {
  run("FOREVER [ WAIT 1 ]");
  ticks(250);  // 0.5 s
  const size_t n = fakes::countTo(4, "logo_pose");
  TEST_ASSERT_TRUE(n >= 4 && n <= 6);
  TEST_ASSERT_TRUE(sent(4, "\"x_cm\":0.0,\"y_cm\":0.0,\"heading\":0"));
}

// Tank Logo LLD §22: GO goes to the motion layer and the program runs on; every tick keeps the GO alive; HOLD is
// sent and ends the keeping; SPEED while going sends GO again at the new speed.
void test_go_is_kept_alive_every_tick_until_hold() {
  run("GO WAIT 1 HOLD BEEP");
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[0].kind);
  TEST_ASSERT_EQUAL(40, g_motions[0].speed_pct);
  ticks(100);  // the program waits; the GO drives on
  TEST_ASSERT_TRUE(g_go_keeps >= 99);
  TEST_ASSERT_EQUAL(1, g_motions.size());
  ticks(500);  // the WAIT is over: HOLD
  TEST_ASSERT_EQUAL(2, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kHold, g_motions[1].kind);
  const int kept = g_go_keeps;
  ticks(10);
  TEST_ASSERT_EQUAL(kept, g_go_keeps);  // held: nothing more to keep alive
}

void test_speed_while_going_sends_go_again() {
  run("GO SPEED 60 WAIT 1");
  ticks(3);
  TEST_ASSERT_EQUAL(2, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[1].kind);
  TEST_ASSERT_EQUAL(60, g_motions[1].speed_pct);
}

void test_speed_without_go_sends_nothing() {
  run("SPEED 60 WAIT 1");
  ticks(3);
  TEST_ASSERT_EQUAL(0, g_motions.size());
}

// Owner 2026-10-22 (final review I1, option a): in step mode a pause on a line holds the GO (the tank stops);
// STEP drives on again at the same speed. HOLD's own end during the pause does not end the GO for the program.
void test_step_mode_holds_a_go_and_step_drives_on() {
  run("SPEED 30\nGO\nWAIT UNTIL DISTANCE < 30\nHOLD", true);
  ticks(1);   // paused on line 1
  lc->onStep(3, 6);
  ticks(2);   // SPEED 30, paused on line 2
  lc->onStep(3, 7);
  ticks(3);   // GO; paused on line 3; the next tick holds the GO
  TEST_ASSERT_EQUAL(2, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[0].kind);
  TEST_ASSERT_EQUAL(logo::ActKind::kHold, g_motions[1].kind);  // the pause stops the tank
  core::PrimitiveEnd held;
  held.kind = core::PrimKind::kGo;
  held.ok = true;
  lc->onPrimitiveEnd(held, g_now);  // HOLD's own end comes back
  const int kept = g_go_keeps;
  ticks(20);
  TEST_ASSERT_EQUAL(kept, g_go_keeps);  // nothing to keep alive while paused
  TEST_ASSERT_TRUE(lc->running());
  lc->onStep(3, 8);
  ticks(1);
  TEST_ASSERT_EQUAL(3, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[2].kind);  // drives on again
  TEST_ASSERT_EQUAL(30, g_motions[2].speed_pct);
  ticks(1);
  TEST_ASSERT_TRUE(g_go_keeps > kept);  // kept alive again from the next tick
}

void test_the_program_s_end_stops_a_go() {
  run("GO");
  ticks(3);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_ended\",\"reason\":\"finished\""));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_TRUE(g_stop_owned);
}

void test_losing_the_drive_during_a_go_never_stops_the_new_driver() {
  run("GO WAIT 5");
  ticks(3);
  g_keep = core::Decision::kNackNotOwner;  // another phone took over
  ticks(1);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_FALSE(g_stop_owned);  // no STOP for the new driver
}

void test_arc_speed_note() {
  run("SPEED 100 ARC RIGHT 90 40");
  ticks(1);
  lc->onUnoDone(0, g_now);
  ticks(2);
  TEST_ASSERT_EQUAL(logo::ActKind::kArc, g_motions.back().kind);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_note\""));
  TEST_ASSERT_TRUE(sent(4, "for this radius"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_run_starts_and_carries_out_moves);
  RUN_TEST(test_run_refusals);
  RUN_TEST(test_light_waits_for_the_uno_s_ack);
  RUN_TEST(test_no_answer_ends_with_7034_a_refusal_goes_on);
  RUN_TEST(test_a_full_queue_is_tried_again);
  RUN_TEST(test_say_then_a_pause);
  RUN_TEST(test_play_goes_in_chunks_of_ten_after_each_other);
  RUN_TEST(test_look_angles_and_home);
  RUN_TEST(test_stop_while_waiting_ends_once);
  RUN_TEST(test_another_device_taking_the_drive_ends_it_without_stopping_them);
  RUN_TEST(test_a_waiting_program_keeps_the_drive_every_tick);
  RUN_TEST(test_the_body_gone_ends_it_with_7034);
  RUN_TEST(test_logo_stop_from_any_page_and_not_running);
  RUN_TEST(test_a_motion_that_cannot_start_ends_it);
  RUN_TEST(test_battery_and_tilt_alarms);
  RUN_TEST(test_step_mode);
  RUN_TEST(test_pose_ten_times_a_second);
  RUN_TEST(test_arc_speed_note);
  RUN_TEST(test_go_is_kept_alive_every_tick_until_hold);
  RUN_TEST(test_speed_while_going_sends_go_again);
  RUN_TEST(test_speed_without_go_sends_nothing);
  RUN_TEST(test_step_mode_holds_a_go_and_step_drives_on);
  RUN_TEST(test_the_program_s_end_stops_a_go);
  RUN_TEST(test_losing_the_drive_during_a_go_never_stops_the_new_driver);
  return UNITY_END();
}
