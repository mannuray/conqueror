// Plan N3: LogoController with fake hooks - run and its refusals, waiting for the Uno's ACK, SAY's pause, PLAY in
// chunks, LOOK angles, every ending stops the tank and gives the drive back, a takeover, step mode, the pose.
// Plan Z2a Task 6: the AI words through a real SkillHub - questions, skills, places relative to the tank, every lease
// end (skill_end + its why), stale answers / ends dropped, the server's close.
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <vector>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/logo_controller.h"
#include "core/skill_hub.h"
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
logo::Vm g_vm, g_launched;
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
// Plan Z2a: the skill server is fd 7; the hooks' record of the skill's drive primitive.
constexpr int kServer = 7;
core::SkillHub g_hub;
bool g_skill_ok = true;
core::BridgeReason g_skill_why = core::BridgeReason::kBodyOffline;
int g_skill_starts = 0, g_skill_ends = 0, g_skill_fd = -1;
int32_t g_skill_id = -1;
core::EndReason g_skill_end_why = core::EndReason::kDone;
// Plan G1 (bridge LLD R34): game mode's hooks
uint16_t g_game_refuse = 0;
bool g_game_ready = true, g_game_caught = false, g_game_seen = false;
int g_game_ends = 0;
int16_t g_game_backing = 0;
uint8_t (*g_game_cap_fn)() = nullptr;

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
  h.start_skill = [](int fd, int32_t id, uint32_t, core::BridgeReason& why) {
    ++g_skill_starts;
    g_skill_fd = fd;
    g_skill_id = id;
    why = g_skill_why;
    return g_skill_ok;
  };
  h.end_skill = [](core::EndReason why, uint32_t now) {  // as the motion layer: the skill's end comes back at once
    ++g_skill_ends;
    g_skill_end_why = why;
    core::PrimitiveEnd e;
    e.kind = core::PrimKind::kSkill;
    e.reason = why;
    e.id = g_skill_id;
    lc->onPrimitiveEnd(e, now);
  };
  h.game_start = [](bool game, uint32_t) {
    g_game_seen = game;
    return g_game_refuse;
  };
  h.game_ready = [] { return g_game_ready; };
  h.game_caught = [] { return g_game_caught; };
  h.game_end = [](uint32_t) { ++g_game_ends; };
  h.game_backing = [](int16_t cm) { g_game_backing = cm; };
  h.game_cap = [] { return g_game_cap_fn != nullptr ? g_game_cap_fn() : static_cast<uint8_t>(0); };
  lc->setHooks(h);
  lc->setSkillHub(&g_hub);
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

// Plan Z2a: an AI program (the checker's ai caps, no list: literal things are not checked here).
void runAi(const char* src) {
  logo::Caps caps;
  caps.ai = true;
  TEST_ASSERT_TRUE_MESSAGE(logo::checkProgram(src, static_cast<uint16_t>(strlen(src)), caps, g_space), src);
  g_checked = g_space.program;
  lc->onRun(3, 5, 1, false, g_now);
}
// A TELEM_FAST from the Uno: the counts, the yaw (0.01°, + = left) and the tracks' commands.
void fast(int32_t enc, uint16_t yaw, int8_t cmd) {
  proto::TelemFastMsg t{};
  t.dist_mm = 65535;
  t.yaw = yaw;
  t.enc_l = t.enc_r = enc;
  t.spd_cmd_l = t.spd_cmd_r = cmd;
  uint8_t f[64];
  const size_t n = cqtest::frame(t, 3, f);
  unoSends(f, n, g_now);
}
int32_t lastId(int fd) {  // the "id" of the last skill_* message to fd (the pages' broadcasts reach it too)
  std::string m;
  for (const fakes::Sent& x : fakes::world().sent) {
    if (x.fd == fd && x.json.find("\"t\":\"skill_") != std::string::npos) m = x.json;
  }
  const size_t at = m.find("\"id\":");
  return at == std::string::npos ? -1 : static_cast<int32_t>(atol(m.c_str() + at + 5));
}
core::PrimitiveEnd skillEnd(core::EndReason why) {  // the motion layer ended the running skill
  core::PrimitiveEnd e;
  e.kind = core::PrimKind::kSkill;
  e.reason = why;
  e.id = g_skill_id;
  return e;
}
void startedSkill() {  // a skill_start went to the server and the lease is its
  TEST_ASSERT_TRUE(sent(kServer, "\"t\":\"skill_start\""));
  TEST_ASSERT_EQUAL(lastId(kServer), g_hub.leaseId());
  TEST_ASSERT_EQUAL(g_skill_id, g_hub.leaseId());
}
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
  lc = new LogoController(out, logbook, *link, *tel, g_running, g_vm, g_launched);
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
  ws.add(kServer, 0);
  g_hub = core::SkillHub();
  g_hub.onHello(kServer);
  g_skill_ok = true;
  g_skill_why = core::BridgeReason::kBodyOffline;
  g_skill_starts = g_skill_ends = 0;
  g_skill_fd = -1;
  g_skill_id = -1;
  g_game_refuse = 0;
  g_game_ready = true;
  g_game_caught = g_game_seen = false;
  g_game_ends = 0;
  g_game_backing = 0;
  g_game_cap_fn = nullptr;
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

// ---- Plan Z2a Task 6: the AI words (bridge LLD R26.4, Tank Logo LLD §24.2) ----

// Review Focus 4: FORWARD 50 then SETXY 0 0 goes back, not on.
void test_forward_then_setxy_back() {
  runAi("FORWARD 50  SETXY 0 0");
  ticks(1);
  fast(3757, 0, 40);  // 50 cm at 7514 counts per metre
  done();
  ticks(1);
  startedSkill();
  TEST_ASSERT_EQUAL(3, g_skill_fd);  // the program's phone holds the body
  TEST_ASSERT_TRUE(sent(kServer, "\"skill\":\"setxy\",\"to\":{\"fwd_cm\":-50,\"left_cm\":0}"));
}

// Review Focus 4: after RIGHT 90 the start is on the tank's right (the tank's yaw is + = left).
void test_right_turn_then_setxy_is_on_the_right() {
  runAi("FORWARD 50  RIGHT 90  SETXY 0 0");
  ticks(1);
  fast(3757, 0, 40);
  done();
  ticks(1);
  fast(3757, 36000 - 9000, 0);  // turned right 90° on the spot
  done();
  ticks(1);
  TEST_ASSERT_TRUE(sent(kServer, "\"to\":{\"fwd_cm\":0,\"left_cm\":-50}"));
}

void test_area_anchor_sent_relative() {
  runAi("AREA 2  FORWARD 50  FIND \"ball");
  ticks(2);  // AREA, then FORWARD
  fast(3757, 0, 40);
  done();
  ticks(1);
  startedSkill();
  TEST_ASSERT_TRUE(sent(kServer, "\"skill\":\"find\",\"thing\":\"ball\",\"area\":{\"r_m\":2,\"fwd_cm\":-50,\"left_cm\":0}"));
}

void test_ask_answer_pushes_true() {
  runAi("IF SEE? \"ball [ BEEP ]");
  ticks(1);
  TEST_ASSERT_TRUE(sent(kServer, "\"t\":\"skill_ask\""));
  TEST_ASSERT_TRUE(sent(kServer, "\"q\":\"see?\",\"thing\":\"ball\""));
  lc->onSkillAnswer(kServer, lastId(kServer), 1, g_now + 10);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());  // BEEP
}

void test_ask_timeout_pushes_false() {
  runAi("IF SEE? \"ball [ BEEP ]");
  ticks(1);
  const int32_t id = lastId(kServer);
  ticks(160);  // 320 ms: no answer is FALSE
  TEST_ASSERT_EQUAL(0, g_ops.size());
  TEST_ASSERT_FALSE(lc->running());
  lc->onSkillAnswer(kServer, id, 1, g_now);  // too late: dropped
  TEST_ASSERT_EQUAL(0, g_ops.size());
}

// Review Focus 2: an answer to question N while N+1 waits is dropped; N+1's own answer is used.
void test_answer_to_an_old_question_is_dropped() {
  runAi("IF SEE? \"ball [ BEEP ]  IF SEE? \"ball [ BEEP ]");
  ticks(1);
  const int32_t first = lastId(kServer);
  ticks(160);  // the first one lapses (FALSE); the second is asked
  const int32_t second = lastId(kServer);
  TEST_ASSERT_TRUE(second > first);
  lc->onSkillAnswer(kServer, first, 1, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(0, g_ops.size());
  lc->onSkillAnswer(4, second, 1, g_now);  // not the server: dropped
  ticks(1);
  TEST_ASSERT_EQUAL(0, g_ops.size());
  lc->onSkillAnswer(kServer, second, 1, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
}

// Plan G3 (bridge LLD R34.3): the game words go out as skills, a person question carries "game":true.
void test_face_starts_a_skill_and_its_done_goes_on() {
  runAi("FACE \"baby  BEEP");
  ticks(1);
  TEST_ASSERT_TRUE(g_game_seen);  // a game run: game mode before the first motion
  startedSkill();
  TEST_ASSERT_TRUE(sent(kServer, "\"skill\":\"face\",\"thing\":\"baby\""));
  lc->onSkillEnd(kServer, g_skill_id, logo::SkillEnd::kDone, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());  // BEEP
}

void test_escape_and_follow_carry_their_numbers() {
  runAi("ESCAPE \"kid 40  FOLLOW \"person 5");
  ticks(1);
  startedSkill();
  TEST_ASSERT_TRUE(sent(kServer, "\"skill\":\"escape\",\"thing\":\"kid\",\"cm\":40"));
  lc->onSkillEnd(kServer, g_skill_id, logo::SkillEnd::kDone, g_now);
  ticks(1);
  startedSkill();
  TEST_ASSERT_TRUE(sent(kServer, "\"skill\":\"follow\",\"thing\":\"person\",\"s\":5"));
}

void test_a_person_question_in_a_game_says_so() {
  runAi("IF SEE? \"baby [ BEEP ]");
  ticks(1);
  TEST_ASSERT_TRUE(sent(kServer, "\"q\":\"see?\",\"thing\":\"baby\",\"game\":true"));
}

void test_a_thing_question_never_says_game() {
  runAi("IF CAUGHT? [ BEEP ]  IF SEE? \"ball [ BEEP ]");
  ticks(2);
  TEST_ASSERT_TRUE(sent(kServer, "\"thing\":\"ball\""));
  TEST_ASSERT_FALSE(sent(kServer, "\"game\":true"));
}

void test_skill_done_goes_on_without_skill_end() {
  runAi("FIND \"ball  BEEP");
  ticks(1);
  startedSkill();
  lc->onSkillEnd(kServer, g_skill_id, logo::SkillEnd::kDone, g_now);
  TEST_ASSERT_EQUAL(1, g_skill_ends);
  TEST_ASSERT_EQUAL(core::EndReason::kDone, g_skill_end_why);
  TEST_ASSERT_EQUAL(-1, g_hub.leaseId());
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());  // BEEP: the program went on
  TEST_ASSERT_FALSE(sent(kServer, "skill_end"));  // the server ended it itself
}

void test_skill_failed_ends_the_program() {
  runAi("FIND \"ball  BEEP");
  ticks(1);
  lc->onSkillEnd(kServer, g_skill_id, logo::SkillEnd::kNotFound, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7050"));
  TEST_ASSERT_EQUAL(1, g_skill_ends);
  TEST_ASSERT_FALSE(sent(kServer, "skill_end"));
}

// Review Focus 2: a second end for skill N while skill N+1 runs never ends N+1; an end from another fd neither.
void test_a_second_end_for_an_old_skill_is_dropped() {
  runAi("FIND \"ball  FIND \"ball");
  ticks(1);
  const int32_t first = g_skill_id;
  lc->onSkillEnd(kServer, first, logo::SkillEnd::kDone, g_now);
  ticks(1);
  const int32_t second = g_skill_id;
  TEST_ASSERT_TRUE(second > first);
  TEST_ASSERT_EQUAL(second, g_hub.leaseId());
  lc->onSkillEnd(kServer, first, logo::SkillEnd::kNotFound, g_now);
  lc->onSkillEnd(4, second, logo::SkillEnd::kNotFound, g_now);
  TEST_ASSERT_TRUE(lc->running());
  TEST_ASSERT_EQUAL(second, g_hub.leaseId());
  lc->onSkillEnd(kServer, second, logo::SkillEnd::kDone, g_now);
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":\"finished\""));
}

// STOP from a page: the motion layer ends the skill (kStop) first, then the MotionEvent follows.
void test_stop_ends_lease_and_sends_skill_end_stop() {
  runAi("WANDER 10");
  ticks(1);
  const int32_t id = g_skill_id;
  lc->onPrimitiveEnd(skillEnd(core::EndReason::kStop), g_now);
  lc->onMotionEvent(core::MotionEvent::kStop, g_now);
  TEST_ASSERT_EQUAL(1, fakes::countTo(kServer, "\"t\":\"skill_end\""));
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"stop\""));
  TEST_ASSERT_EQUAL(id, lastId(kServer));
  TEST_ASSERT_EQUAL(-1, g_hub.leaseId());
  TEST_ASSERT_FALSE(g_hub.leaseOk(kServer, id, g_now));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7040"));
  TEST_ASSERT_EQUAL(1, g_stops);
}

void test_logo_stop_sends_skill_end_stop() {
  runAi("WANDER 10");
  ticks(1);
  lc->onStop(4, 9, g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"stop\""));
  TEST_ASSERT_EQUAL(1, g_skill_ends);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7040"));
}

void test_page_hidden_sends_skill_end_page() {
  runAi("WANDER 10");
  ticks(1);
  lc->onPrimitiveEnd(skillEnd(core::EndReason::kCancelled), g_now);
  TEST_ASSERT_TRUE(lc->running());  // waits for the MotionEvent behind it
  lc->onMotionEvent(core::MotionEvent::kOwnerClosed, g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"page\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7046"));
}

void test_owner_stick_sends_skill_end_owner() {
  runAi("WANDER 10");
  ticks(1);
  lc->onPrimitiveEnd(skillEnd(core::EndReason::kCancelled), g_now);
  lc->onMotionEvent(core::MotionEvent::kOwnerCommand, g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"owner\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7045"));
}

void test_another_driver_taking_over_is_owner() {
  runAi("WANDER 10");
  ticks(1);
  g_keep = core::Decision::kNackNotOwner;
  ticks(1);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"owner\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7045"));
}

// Review Focus 3: the engine killed (its socket closes) mid-skill: 7052, no skill_end, no lease for a reconnect.
void test_server_close_mid_skill_is_7052() {
  runAi("WANDER 10");
  ticks(1);
  const int32_t id = g_skill_id;
  lc->onClientClose(kServer, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7052"));
  TEST_ASSERT_EQUAL(1, g_skill_ends);
  TEST_ASSERT_FALSE(sent(kServer, "skill_end"));
  TEST_ASSERT_EQUAL(-1, g_hub.server());
  g_hub.onHello(kServer);  // it comes back on the same fd
  TEST_ASSERT_FALSE(g_hub.leaseOk(kServer, id, g_now));
}

// A silent server reaped without a kWsClose (DriveController's ghost path): seen on the next tick.
void test_a_server_gone_without_a_close_is_seen_on_tick() {
  runAi("WANDER 10");
  ticks(1);
  ws.remove(kServer);
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7052"));
  TEST_ASSERT_EQUAL(-1, g_hub.server());
}

// Review Focus 3: the WiFi drops - no skill_drive for 300 ms: the lease's deadman ends it 7052 with skill_end deadman.
void test_lease_deadman_is_7052() {
  runAi("WANDER 10");
  ticks(1);
  ticks(149);  // 298 ms
  TEST_ASSERT_TRUE(lc->running());
  ticks(3);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"deadman\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7052"));
  TEST_ASSERT_EQUAL(1, g_skill_ends);
}

// The motion layer's own deadman fires in the same loop: one end, one skill_end.
void test_motion_deadman_then_hub_deadman_end_once() {
  runAi("WANDER 10");
  ticks(1);
  g_now += 302;
  lc->onPrimitiveEnd(skillEnd(core::EndReason::kDeadman), g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(1, fakes::countTo(kServer, "\"why\":\"deadman\""));
  TEST_ASSERT_EQUAL(1, fakes::countTo(4, "logo_ended"));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7052"));
}

void test_brake_ends_the_skill_blocked() {
  runAi("WANDER 10");
  ticks(1);
  lc->onPrimitiveEnd(skillEnd(core::EndReason::kBlocked), g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"ended\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7024"));
}

void test_a_stall_ends_the_skill_7044() {
  runAi("WANDER 10");
  ticks(1);
  lc->onPrimitiveEnd(skillEnd(core::EndReason::kStalled), g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"ended\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7044"));
}

void test_an_old_skill_s_primitive_end_is_dropped() {
  runAi("WANDER 10");
  ticks(1);
  core::PrimitiveEnd e = skillEnd(core::EndReason::kBlocked);
  e.id = g_skill_id - 1;
  lc->onPrimitiveEnd(e, g_now);
  TEST_ASSERT_TRUE(lc->running());
}

void test_battery_during_a_skill_sends_skill_end_ended() {
  runAi("WANDER 10");
  ticks(1);
  lc->onUnoAlarm(logo::RunEvent::kBatteryLow, g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"ended\""));
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7042"));
  TEST_ASSERT_EQUAL(1, g_skill_ends);
  TEST_ASSERT_EQUAL(-1, g_hub.leaseId());
}

// bridge LLD R26.1 / Task 5 carry: startSkill replaces a primitive without its end - a GO is held first.
void test_a_go_is_held_before_the_skill_starts() {
  runAi("GO  FIND \"ball");
  ticks(2);
  TEST_ASSERT_TRUE(g_motions.size() >= 2);
  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[0].kind);
  TEST_ASSERT_EQUAL(logo::ActKind::kHold, g_motions.back().kind);
  TEST_ASSERT_EQUAL(1, g_skill_starts);
}

void test_no_server_is_7052() {
  g_hub.onClose(kServer);
  runAi("FIND \"ball");
  ticks(1);
  TEST_ASSERT_EQUAL(0, g_skill_starts);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7052"));
}

void test_a_refused_skill_drive_is_7021_and_no_lease() {
  g_skill_ok = false;
  g_skill_why = core::BridgeReason::kUnavailable;
  runAi("FIND \"ball");
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7021"));
  TEST_ASSERT_EQUAL(-1, g_hub.leaseId());
  TEST_ASSERT_FALSE(sent(kServer, "skill_start"));
}

void test_skill_messages_without_a_program_are_dropped() {
  lc->onSkillAnswer(kServer, 1, 1, g_now);
  lc->onSkillEnd(kServer, 1, logo::SkillEnd::kDone, g_now);
  lc->onClientClose(kServer, g_now);
  TEST_ASSERT_EQUAL(0, g_skill_ends);
  TEST_ASSERT_EQUAL(-1, g_hub.server());
}

// ---- Plan G1 (bridge LLD R34, Tank Logo LLD §27): game mode's hooks

void test_a_game_run_tells_game_mode_and_a_refusal_ends_it_with_the_code() {
  g_game_refuse = 7061;
  run("IF CAUGHT? [ BEEP ] FD 10");
  TEST_ASSERT_TRUE(g_game_seen);  // the checker's flag reached game mode
  ticks(2);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_EQUAL(0, g_motions.size());
  TEST_ASSERT_TRUE(sent(4, "7061"));
  TEST_ASSERT_EQUAL(1, g_game_ends);
}

void test_a_non_game_run_is_not_a_game() {
  run("FD 10");
  TEST_ASSERT_FALSE(g_game_seen);
}

void test_a_game_waits_until_armed() {
  g_game_ready = false;
  run("IF CAUGHT? [ BEEP ] FD 10");
  ticks(5);
  TEST_ASSERT_EQUAL(0, g_motions.size());
  g_game_ready = true;
  ticks(2);
  TEST_ASSERT_EQUAL(1, g_motions.size());
}

void test_caught_reaches_the_program() {
  g_game_caught = true;
  run("IFELSE CAUGHT? [ BEEP ] [ FD 10 ]");
  ticks(2);
  TEST_ASSERT_EQUAL(0, g_motions.size());
  TEST_ASSERT_EQUAL(1, g_ops.size());  // the BEEP
}

void test_a_backward_move_tells_game_mode_its_length() {
  run("IF CAUGHT? [ BEEP ] BK 20");
  ticks(2);
  TEST_ASSERT_EQUAL_INT16(20, g_game_backing);
  TEST_ASSERT_EQUAL(1, g_motions.size());
}

void test_game_mode_ends_the_run_with_its_code() {
  run("IF CAUGHT? [ BEEP ] FD 10");
  ticks(1);
  lc->endWith(7052, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "7052"));
  TEST_ASSERT_EQUAL(1, g_game_ends);
}

void test_caught_ends_a_running_skill_as_caught_and_the_program_goes_on() {
  runAi("WANDER 5 FD 10");
  ticks(1);
  startedSkill();
  lc->onCaught(g_now);
  TEST_ASSERT_TRUE(sent(kServer, "\"why\":\"caught\""));
  ticks(20);
  TEST_ASSERT_TRUE(lc->running());  // not an error
  // G3 final review C1: the program goes on - FD 10 runs (the first motion is the HOLD a skill starts with)
  TEST_ASSERT_EQUAL(2, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kMove, g_motions[1].kind);
}

void test_caught_during_a_game_word_lets_lesson_6_go_on() {  // G3 final review C1: CAUGHT? then runs
  runAi("FOLLOW \"baby 2  IF CAUGHT? [ BEEP ]");
  ticks(1);
  startedSkill();
  g_game_caught = true;
  lc->onCaught(g_now);
  ticks(20);
  TEST_ASSERT_TRUE(lc->running());
  TEST_ASSERT_EQUAL(1, g_ops.size());  // BEEP: the word ended and CAUGHT? was read
}

void test_a_games_moves_are_within_its_speed_cap() {  // review I7
  g_game_cap_fn = [] { return static_cast<uint8_t>(50); };  // a game at a 50 % cap
  run("SPEED 100 FD 10");
  ticks(3);
  TEST_ASSERT_EQUAL(1, g_motions.size());
  TEST_ASSERT_EQUAL_UINT8(50, g_motions[0].speed_pct);
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
  RUN_TEST(test_forward_then_setxy_back);
  RUN_TEST(test_right_turn_then_setxy_is_on_the_right);
  RUN_TEST(test_area_anchor_sent_relative);
  RUN_TEST(test_ask_answer_pushes_true);
  RUN_TEST(test_ask_timeout_pushes_false);
  RUN_TEST(test_answer_to_an_old_question_is_dropped);
  RUN_TEST(test_face_starts_a_skill_and_its_done_goes_on);
  RUN_TEST(test_escape_and_follow_carry_their_numbers);
  RUN_TEST(test_a_person_question_in_a_game_says_so);
  RUN_TEST(test_a_thing_question_never_says_game);
  RUN_TEST(test_skill_done_goes_on_without_skill_end);
  RUN_TEST(test_skill_failed_ends_the_program);
  RUN_TEST(test_a_second_end_for_an_old_skill_is_dropped);
  RUN_TEST(test_stop_ends_lease_and_sends_skill_end_stop);
  RUN_TEST(test_logo_stop_sends_skill_end_stop);
  RUN_TEST(test_page_hidden_sends_skill_end_page);
  RUN_TEST(test_owner_stick_sends_skill_end_owner);
  RUN_TEST(test_another_driver_taking_over_is_owner);
  RUN_TEST(test_server_close_mid_skill_is_7052);
  RUN_TEST(test_a_server_gone_without_a_close_is_seen_on_tick);
  RUN_TEST(test_lease_deadman_is_7052);
  RUN_TEST(test_motion_deadman_then_hub_deadman_end_once);
  RUN_TEST(test_brake_ends_the_skill_blocked);
  RUN_TEST(test_a_stall_ends_the_skill_7044);
  RUN_TEST(test_an_old_skill_s_primitive_end_is_dropped);
  RUN_TEST(test_battery_during_a_skill_sends_skill_end_ended);
  RUN_TEST(test_a_go_is_held_before_the_skill_starts);
  RUN_TEST(test_no_server_is_7052);
  RUN_TEST(test_a_refused_skill_drive_is_7021_and_no_lease);
  RUN_TEST(test_skill_messages_without_a_program_are_dropped);
  RUN_TEST(test_a_game_run_tells_game_mode_and_a_refusal_ends_it_with_the_code);
  RUN_TEST(test_a_non_game_run_is_not_a_game);
  RUN_TEST(test_a_game_waits_until_armed);
  RUN_TEST(test_caught_reaches_the_program);
  RUN_TEST(test_a_backward_move_tells_game_mode_its_length);
  RUN_TEST(test_game_mode_ends_the_run_with_its_code);
  RUN_TEST(test_caught_ends_a_running_skill_as_caught_and_the_program_goes_on);
  RUN_TEST(test_caught_during_a_game_word_lets_lesson_6_go_on);
  RUN_TEST(test_a_games_moves_are_within_its_speed_cap);
  return UNITY_END();
}
