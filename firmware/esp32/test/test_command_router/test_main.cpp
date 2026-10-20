// CommandRouter: parse, STOP fast path, admission, one table of routes (bridge LLD R15.4, R15.6).
#include <string.h>
#include <unity.h>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/command_router.h"
#include "fakes/fake_ports.h"

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
logo::Vm g_logo_vm;
app::LogoController* logoc = nullptr;
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
  logoc = new app::LogoController(out, logbook, *link, *tel, g_logo_program, g_logo_vm);
  router = new app::CommandRouter(*drive, *motion, *settings, *tank, *logoc, *tel, out);
  router->setHello("abc1234", sensor);
  motion->setHooks({[](int fd, uint32_t now) { return drive->claim(fd, now); }, [] { return drive->otaActive(); }});
  tel->setViews({[] { return drive->ownerView(); }, [] { return motion->view(); }, [] { return !link->synced(); }});
  settings->setHooks({[] { return motion->active(); }, [] { return drive->otaActive(); }, [] { return owner->driverFd(); },
                      [](int fd) { motion->sendCalib(fd); }, [](const core::AckedOp& op) { return uno->request(op); },
                      &motion->cpm(), &motion->rollMm(), [] { return uno->freeSlots(); }});
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

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_kind_has_a_route_except_the_invalid_ones);
  RUN_TEST(test_unknown_command_is_invalid);
  RUN_TEST(test_ping_answers_pong);
  RUN_TEST(test_hello_answers_welcome_with_the_sensor);
  RUN_TEST(test_stop_bypasses_admission_even_for_an_unknown_client);
  RUN_TEST(test_commands_from_an_unknown_client_are_dropped);
  RUN_TEST(test_ota_answers_every_waiter);
  RUN_TEST(test_logo_commands_are_routed);
  return UNITY_END();
}
