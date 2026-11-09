// CommandRouter: parse, STOP fast path, admission, one table of routes (bridge LLD R15.4, R15.6).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/command_router.h"
#include "core/ai_pages.h"
#include "core/skill_hub.h"
#include "fakes/fake_ports.h"
#include "logo/check_job.h"

using namespace cq;
using FT = proto::FrameType;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::EventLog logbook;
core::FrameTx* tx = nullptr;
core::BodyLink* link = nullptr;
core::LinkOwner* owner = nullptr;
app::TelemetryPublisher* tel = nullptr;
app::UnoChannel* uno = nullptr;
app::MotionController* motion = nullptr;
app::SettingsController* settings = nullptr;
app::TankAppController* tank = nullptr;
app::DriveController* drive = nullptr;
app::CommandRouter* router = nullptr;
logo::Program g_logo_program;
logo::Vm g_logo_vm, g_logo_launched;
app::LogoController* logoc = nullptr;
core::SkillHub hub;
core::AiPages pages;
cqtest::Wire wire;
uint32_t g_post = 0;

const char* sensor() { return "OV3660"; }
void text(int fd, const char* json, uint32_t now) { router->onText(fd, json, strlen(json), ++g_post, now); }
bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }

void sync() {
  uint8_t f[64];
  size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 1, 1, 0x1UL}, 1, f);
  link->onUartBytes(f, n, 10);
  core::LinkEvent ev{};
  while (link->popEvent(ev)) {
    if (ev.kind == core::LinkEventKind::kBodyOnline) drive->onSynced(true, 10);
  }
  wire.drain(*tx);
  wire.clear();
}
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  logbook = core::EventLog();
  delete router; delete logoc; delete drive; delete tank; delete settings; delete motion; delete uno; delete tel; delete owner;
  delete link; delete tx;
  tx = new core::FrameTx();
  link = new core::BodyLink(*tx);
  owner = new core::LinkOwner();
  tel = new app::TelemetryPublisher(out, *link);
  uno = new app::UnoChannel(*link, *tx, out);
  motion = new app::MotionController(out, logbook, *link, *owner, *tel);
  settings = new app::SettingsController(out, logbook, *link);
  tank = new app::TankAppController(out, logbook, *link);
  drive = new app::DriveController(out, logbook, *owner, *uno, *motion, *settings, *tel);
  logoc = new app::LogoController(out, logbook, *link, *tel, g_logo_program, g_logo_vm, g_logo_launched);
  router = new app::CommandRouter(*drive, *motion, *settings, *tank, *logoc, *tel, out);
  router->setHello("abc1234", sensor);
  hub = core::SkillHub();
  router->setSkillHub(&hub);
  pages = core::AiPages();
  router->setAiPages(&pages);
  logoc->setSkillHub(&hub);
  app::MotionController::Hooks mh{[](int fd, uint32_t now, bool moving) { return drive->claim(fd, now, moving); },
                                  [] { return drive->otaActive(); }};
  mh.lease_ok = [](int fd, int32_t id, uint32_t now) { return hub.leaseOk(fd, id, now); };
  motion->setHooks(mh);
  tel->setViews({[] { return drive->ownerView(); }, [] { return motion->view(); }, [] { return !link->synced(); }});
  settings->setHooks({[] { return motion->active(); }, [] { return drive->otaActive(); }, [] { return owner->driverFd(); },
                      [](int fd) { motion->sendCalib(fd); }, [](const core::AckedOp& op) { return uno->request(op); },
                      &motion->cpm(), &motion->rollMm(), [] { return uno->freeSlots(); }, &motion->coastMs()});
  g_post = 0;
  drive->begin(0);
  drive->onClientOpen(3, 0);
}
void tearDown() {}

void test_every_kind_has_a_route_except_the_invalid_ones() {
  for (uint8_t k = 0; k <= static_cast<uint8_t>(core::WsCmdKind::kInvalid); ++k) {
    const auto kind = static_cast<core::WsCmdKind>(k);
    const bool invalid = kind == core::WsCmdKind::kUnsupported || kind == core::WsCmdKind::kInvalid;
    const bool stop = kind == core::WsCmdKind::kStop;  // the fast path, before the table
    TEST_ASSERT_EQUAL_MESSAGE(!invalid && !stop, app::CommandRouter::routes(kind), "a command kind without a route");
  }
}

void test_a_bad_skill_field_is_bad_field() {
  text(3, "{\"t\":\"skill_done\"}", 100);
  TEST_ASSERT_TRUE(has(3, "BAD_FIELD"));
}

void test_unknown_command_is_invalid() {
  text(3, "{\"t\":\"fly\",\"id\":4}", 100);
  TEST_ASSERT_TRUE(has(3, "INVALID_COMMAND"));
}

uint64_t fakeClockUs() { return 42000000123ULL; }

void test_ping_answers_pong() {
  text(3, "{\"t\":\"ping\",\"t0\":12}", 100);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"pong\""));
  TEST_ASSERT_TRUE(has(3, "\"bridge_us\":0"));  // no clock set
  router->setClockUs(fakeClockUs);  // Plan Q1: the ESP32 timer, for the AI engine's clock offset
  text(3, "{\"t\":\"ping\",\"t0\":13}", 101);
  TEST_ASSERT_TRUE(has(3, "\"bridge_us\":42000000123"));
}

void test_hello_answers_welcome_with_the_sensor() {
  text(3, "{\"t\":\"hello\",\"client\":\"x\",\"proto\":1}", 100);
  TEST_ASSERT_TRUE(has(3, "OV3660"));
}

void test_stop_bypasses_admission_even_for_an_unknown_client() {
  sync();
  text(9, "{\"t\":\"stop\",\"id\":1}", 100);  // fd 9 never opened (STOP may overtake its own open)
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kStop));
}

void test_commands_from_an_unknown_client_are_dropped() {
  text(9, "{\"t\":\"tank\"}", 100);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
}

void test_ota_answers_every_waiter() {  // Review Focus 5
  sync();
  text(3, "{\"t\":\"setting\",\"setting_id\":66,\"id\":1}", 100);  // queued setting read
  text(3, "{\"t\":\"cam_home\",\"id\":2}", 101);                    // queued behind it
  uno->tick(102);                                                    // the first one in flight
  text(3, "{\"t\":\"stop\",\"id\":3}", 103);                        // a STOP waiter
  drive->startOta(104);
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "\"id\":1"));
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "\"id\":2"));
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "\"id\":3"));
  TEST_ASSERT_EQUAL(1, fakes::world().closed.size());
  wire.drain(*tx);
  TEST_ASSERT_TRUE(wire.count(FT::kStop) >= 1);
}

// Plan N3: the logo_* commands reach the LogoController (nothing runs: NOT_RUNNING).
void test_logo_commands_are_routed() {
  text(3, "{\"t\":\"logo_stop\",\"id\":7}", 100);
  TEST_ASSERT_TRUE(fakes::countTo(3, "NOT_RUNNING") == 1);
  text(3, "{\"t\":\"logo_step\",\"id\":8}", 102);
  TEST_ASSERT_TRUE(fakes::countTo(3, "NOT_RUNNING") == 2);
}

// ---- Plan Z2a Task 6: the skill server's messages (bridge LLD R26.2) ----
void hello(int fd) { text(fd, "{\"t\":\"hello\",\"client\":\"ai-skills\",\"proto\":1}", 100); }

void test_hello_ai_skills_registers_the_one_server() {
  drive->onClientOpen(4, 0);
  hello(3);
  TEST_ASSERT_EQUAL(3, hub.server());
  TEST_ASSERT_TRUE(has(3, "\"t\":\"welcome\""));
  hello(4);
  TEST_ASSERT_TRUE(has(4, "BUSY"));
  TEST_ASSERT_FALSE(has(4, "\"t\":\"welcome\""));
  TEST_ASSERT_EQUAL(3, hub.server());
  text(4, "{\"t\":\"hello\",\"client\":\"phone\",\"proto\":1}", 101);  // a page's hello is not a server's
  TEST_ASSERT_TRUE(has(4, "\"t\":\"welcome\""));
}

void test_skill_things_reach_the_hub() {
  hello(3);
  text(3, "{\"t\":\"skill_things\",\"id\":2,\"part\":1,\"of\":1,\"things\":[{\"word\":\"toy\",\"names\":[\"car\"]}]}", 102);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":2"));
  TEST_ASSERT_TRUE(hub.commitPending());
  const core::LockPort free_lock{[](void*, uint32_t) { return true; }, [](void*) {}, nullptr};
  TEST_ASSERT_TRUE(hub.tryCommit(free_lock));
  TEST_ASSERT_EQUAL_STRING("toy", hub.things().resolve("car"));
}

// Plan Z2b-1 (bridge LLD R27.1): the engine's page port with the address its socket comes from; skill_learning.
void test_hello_page_keeps_the_feed_address() {
  fakes::world().peer_ip = "192.168.1.23";
  text(3, "{\"t\":\"hello\",\"client\":\"ai-skills\",\"proto\":1,\"page\":{\"port\":8770,\"proto\":1}}", 100);
  TEST_ASSERT_EQUAL_STRING("ws://192.168.1.23:8770/feed", hub.pageUrl());
}

void test_skill_learning_routed_to_hub() {
  drive->onClientOpen(4, 0);
  hello(3);
  text(4, "{\"t\":\"skill_learning\",\"on\":true}", 101);  // not the server: ignored
  TEST_ASSERT_FALSE(hub.learning());
  text(3, "{\"t\":\"skill_learning\",\"on\":true}", 102);
  TEST_ASSERT_TRUE(hub.learning());
  text(3, "{\"t\":\"skill_learning\",\"on\":false}", 103);
  TEST_ASSERT_FALSE(hub.learning());
}

void test_ai_page_hello_gets_welcome_and_ai_at() {
  fakes::world().peer_ip = "192.168.1.23";
  drive->onClientOpen(4, 0);
  text(3, "{\"t\":\"hello\",\"client\":\"ai-skills\",\"proto\":1,\"page\":{\"port\":8770,\"proto\":1}}", 100);
  text(4, "{\"t\":\"hello\",\"client\":\"ai-page\",\"proto\":1}", 101);
  TEST_ASSERT_TRUE(has(4, "\"t\":\"welcome\""));
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ai_at\",\"url\":\"ws://192.168.1.23:8770/feed\",\"proto\":1}", fakes::lastTo(4).c_str());
  TEST_ASSERT_TRUE(pages.contains(4));
}

void test_other_clients_get_no_ai_at() {
  text(3, "{\"t\":\"hello\",\"client\":\"basic-drive\",\"proto\":1}", 100);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"welcome\""));
  TEST_ASSERT_FALSE(has(3, "ai_at"));
  TEST_ASSERT_FALSE(pages.contains(3));
}

void test_ai_page_without_engine_gets_null() {
  text(3, "{\"t\":\"hello\",\"client\":\"ai-page\",\"proto\":1}", 100);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ai_at\",\"url\":null}", fakes::lastTo(3).c_str());
}

void test_skill_things_refusals() {
  drive->onClientOpen(4, 0);
  hello(3);
  text(3, "{\"t\":\"skill_things\",\"id\":3,\"part\":2,\"of\":2,\"things\":[{\"word\":\"toy\"}]}", 102);
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "BAD_FIELD"));  // out of order
  text(3, "{\"t\":\"skill_things\",\"id\":4,\"part\":1,\"of\":1,\"things\":[{\"word\":7}]}", 103);
  TEST_ASSERT_EQUAL(2, fakes::countTo(3, "BAD_FIELD"));  // not a list of words
  text(3, "{\"t\":\"skill_things\",\"id\":5,\"part\":1,\"of\":1,\"things\":[{\"word\":\"abcdefghijklmnopqrstuvwxyz\"}]}", 104);
  TEST_ASSERT_TRUE(has(3, "TOO_BIG"));  // a word over 24 bytes
  text(4, "{\"t\":\"skill_things\",\"id\":6,\"part\":1,\"of\":1,\"things\":[{\"word\":\"toy\"}]}", 105);
  TEST_ASSERT_TRUE(has(4, "NOT_OWNER"));  // not the server
  TEST_ASSERT_FALSE(hub.commitPending());
}

void test_skill_drive_without_a_lease_is_not_owner() {
  hello(3);
  text(3, "{\"t\":\"skill_drive\",\"id\":1,\"v\":40,\"yaw_rate\":10}", 102);
  TEST_ASSERT_TRUE(has(3, "NOT_OWNER"));
}

// A skill_drive posted before a STOP that was handled first is dropped, as a drive_rate (the STOP barrier).
void test_skill_drive_behind_a_stop_is_dropped() {
  hello(3);
  const char* stop = "{\"t\":\"stop\",\"id\":9}";
  router->onText(3, stop, strlen(stop), 20, 102);
  fakes::reset();
  const char* drive_frame = "{\"t\":\"skill_drive\",\"id\":1,\"v\":40,\"yaw_rate\":10}";
  router->onText(3, drive_frame, strlen(drive_frame), 19, 103);
  TEST_ASSERT_FALSE(has(3, "NOT_OWNER"));  // never reached the motion controller
}

// A program runs on phone 3, the server is fd 4: the real JSON of skill_answer / skill_done / skill_failed reaches
// the program (true is TRUE, done goes on, failed's reason is its code).
logo::Program g_checked;
logo::CheckSpace g_space;
int g_beeps = 0;
app::LogoController::Hooks g_hooks;
void aiHooks() {
  app::LogoController::Hooks h;
  h.claim = [](int, uint32_t) { return core::Decision::kSend; };
  h.keep = [](int, uint32_t) { return core::Decision::kSend; };
  h.ota = [] { return false; };
  h.take = [](uint32_t, logo::Program& p, char* name, size_t cap) {
    p = g_checked;
    snprintf(name, cap, "ai");
    return app::LogoController::Took::kOk;
  };
  h.start_motion = [](int, const logo::Action&, uint32_t, core::BridgeReason&) { return true; };
  h.stop_tank = [](bool, uint32_t) {};
  h.release = [](int, uint32_t) {};
  h.uno_request = [](const core::AckedOp&) {
    ++g_beeps;
    return true;
  };
  h.cpm = [] { return static_cast<uint16_t>(7514); };
  h.start_skill = [](int, int32_t, uint32_t, core::BridgeReason&) { return true; };
  h.end_skill = [](core::EndReason, uint32_t) {};
  g_hooks = h;
  logoc->setHooks(h);
}
int32_t lastSkillId(int fd) {
  std::string m;
  for (const fakes::Sent& x : fakes::world().sent) {
    if (x.fd == fd && x.json.find("\"t\":\"skill_") != std::string::npos) m = x.json;
  }
  const size_t at = m.find("\"id\":");
  return at == std::string::npos ? -1 : static_cast<int32_t>(atol(m.c_str() + at + 5));
}
void skillMsg(const char* fmt, int32_t id, uint32_t now) {
  char json[128];
  snprintf(json, sizeof json, fmt, static_cast<long>(id));
  text(4, json, now);
}

void test_skill_messages_reach_the_running_program() {
  drive->onClientOpen(4, 0);
  hello(4);
  aiHooks();
  g_beeps = 0;
  logo::Caps caps;
  caps.ai = true;
  const char* src = "IF SEE? \"ball [ BEEP ]  FIND \"ball  FIND \"ball";
  TEST_ASSERT_TRUE(logo::checkProgram(src, static_cast<uint16_t>(strlen(src)), caps, g_space));
  g_checked = g_space.program;
  logoc->onRun(3, 5, 1, false, 200);
  logoc->tick(202);
  TEST_ASSERT_TRUE(fakes::countTo(4, "\"t\":\"skill_ask\"") == 1);
  skillMsg("{\"t\":\"skill_answer\",\"id\":%ld,\"value\":true}", lastSkillId(4), 210);
  logoc->tick(212);
  TEST_ASSERT_EQUAL(1, g_beeps);  // TRUE: BEEP
  logoc->onUnoDone(0, 214);
  logoc->tick(216);
  TEST_ASSERT_TRUE(fakes::countTo(4, "\"skill\":\"find\"") == 1);
  skillMsg("{\"t\":\"skill_done\",\"id\":%ld}", lastSkillId(4), 220);
  logoc->tick(222);
  TEST_ASSERT_TRUE(fakes::countTo(4, "\"skill\":\"find\"") == 2);  // done: the program went on
  skillMsg("{\"t\":\"skill_failed\",\"id\":%ld,\"reason\":\"lost\",\"thing\":\"ball\"}", lastSkillId(4), 230);
  TEST_ASSERT_FALSE(logoc->running());
  TEST_ASSERT_TRUE(has(3, "\"reason\":7051"));
}

// The body is there with its encoders and the program's phone 3 drives: a skill's drive primitive can start.
void fastFrame(uint32_t now) {
  proto::TelemFastMsg t{};
  t.dist_mm = 65535;
  uint8_t f[64];
  const size_t n = cqtest::frame(t, 2, f);
  link->onUartBytes(f, n, now);
}
void runAiProgram(const char* src, uint32_t now) {
  logo::Caps caps;
  caps.ai = true;
  TEST_ASSERT_TRUE(logo::checkProgram(src, static_cast<uint16_t>(strlen(src)), caps, g_space));
  g_checked = g_space.program;
  logoc->onRun(3, 5, 1, false, now);
}

// skill_answer false: routed - the program goes on at once (a lost answer would wait the 300 ms out).
void test_skill_answer_false_goes_on_at_once() {
  drive->onClientOpen(4, 0);
  hello(4);
  aiHooks();
  g_beeps = 0;
  runAiProgram("IF SEE? \"ball [ BEEP ]  BEEP", 200);
  logoc->tick(202);
  skillMsg("{\"t\":\"skill_answer\",\"id\":%ld,\"value\":false}", lastSkillId(4), 204);
  logoc->tick(206);
  TEST_ASSERT_EQUAL(1, g_beeps);  // FALSE: no BEEP in the IF, the BEEP after it - without the 300 ms wait
}

// skill_drive under the live lease reaches the motion side: its frame drives (DRIVE v 40) and no NOT_OWNER.
void test_skill_drive_under_a_live_lease_drives() {
  sync();
  fastFrame(20);
  drive->onClientOpen(4, 0);
  hello(4);
  aiHooks();
  app::LogoController::Hooks h = g_hooks;
  h.claim = [](int fd, uint32_t now) { return drive->claim(fd, now); };
  h.keep = [](int fd, uint32_t now) { return owner->keepDriving(fd, now); };
  h.start_skill = [](int fd, int32_t id, uint32_t now, core::BridgeReason& why) {
    return motion->startSkill(fd, id, now, why);
  };
  h.end_skill = [](core::EndReason why, uint32_t now) { motion->endSkill(why, now); };
  logoc->setHooks(h);
  runAiProgram("WANDER 10", 200);
  logoc->tick(202);
  TEST_ASSERT_TRUE(motion->active());
  skillMsg("{\"t\":\"skill_drive\",\"id\":%ld,\"v\":40,\"yaw_rate\":0}", lastSkillId(4), 210);
  TEST_ASSERT_FALSE(has(4, "NOT_OWNER"));
  wire.drain(*tx);
  wire.clear();
  fastFrame(220);
  motion->onTelemetry(220);
  wire.drain(*tx);
  const cqtest::Sent* d = wire.last(FT::kDrive);
  TEST_ASSERT_NOT_NULL(d);
  proto::DriveMsg m{};
  TEST_ASSERT_TRUE(proto::decode(d->payload, d->len, m));
  TEST_ASSERT_EQUAL(40, m.v);
}

// Plan G1 (bridge LLD R34): a person's STOP and the engine's answers reach game mode.
int g_game_stops = 0;
int32_t g_game_answer = -1;
void test_stop_and_answers_reach_game_mode() {
  sync();
  app::CommandRouter::GameHooks gh;
  gh.on_stop = [](uint32_t) { ++g_game_stops; };
  gh.on_answer = [](int32_t id, int32_t, uint32_t) { g_game_answer = id; };
  router->setGameHooks(gh);
  text(9, "{\"t\":\"stop\",\"id\":1}", 100);
  TEST_ASSERT_EQUAL(1, g_game_stops);
  ws.add(7, 0);
  hub.onHello(7);
  text(7, "{\"t\":\"skill_answer\",\"id\":42,\"value\":true}", 200);
  TEST_ASSERT_EQUAL(42, g_game_answer);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_kind_has_a_route_except_the_invalid_ones);
  RUN_TEST(test_unknown_command_is_invalid);
  RUN_TEST(test_a_bad_skill_field_is_bad_field);
  RUN_TEST(test_ping_answers_pong);
  RUN_TEST(test_hello_answers_welcome_with_the_sensor);
  RUN_TEST(test_stop_bypasses_admission_even_for_an_unknown_client);
  RUN_TEST(test_commands_from_an_unknown_client_are_dropped);
  RUN_TEST(test_ota_answers_every_waiter);
  RUN_TEST(test_logo_commands_are_routed);
  RUN_TEST(test_hello_ai_skills_registers_the_one_server);
  RUN_TEST(test_skill_things_reach_the_hub);
  RUN_TEST(test_skill_things_refusals);
  RUN_TEST(test_skill_drive_without_a_lease_is_not_owner);
  RUN_TEST(test_skill_answer_false_goes_on_at_once);
  RUN_TEST(test_skill_drive_under_a_live_lease_drives);
  RUN_TEST(test_skill_drive_behind_a_stop_is_dropped);
  RUN_TEST(test_skill_messages_reach_the_running_program);
  RUN_TEST(test_hello_page_keeps_the_feed_address);
  RUN_TEST(test_skill_learning_routed_to_hub);
  RUN_TEST(test_ai_page_hello_gets_welcome_and_ai_at);
  RUN_TEST(test_other_clients_get_no_ai_at);
  RUN_TEST(test_ai_page_without_engine_gets_null);
  RUN_TEST(test_stop_and_answers_reach_game_mode);
  return UNITY_END();
}
