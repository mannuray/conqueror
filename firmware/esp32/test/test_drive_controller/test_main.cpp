// DriveController: drive, STOP (+ hold), release, camera, reaping, OTA - today's rules pinned (bridge LLD R2.7, R14, R15).
#include <string.h>
#include <unity.h>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/drive_controller.h"
#include "core/ws_codec.h"
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
app::DriveController* drive = nullptr;
cqtest::Wire wire;
uint32_t g_post = 0;

core::WsCommand P(const char* s) { return core::parseWsCommand(s, strlen(s)); }
bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
void unoSends(const uint8_t* f, size_t n, uint32_t now) { link->onUartBytes(f, n, now); }

void pump(uint32_t now) {  // the root: link events -> controllers
  core::LinkEvent ev{};
  while (link->popEvent(ev)) {
    if (ev.kind == core::LinkEventKind::kStopResult) uno->onStopResult(ev.status);
    if (ev.kind == core::LinkEventKind::kAckResult) uno->onAck(ev.seq, ev.status);
    if (ev.kind == core::LinkEventKind::kBodyOnline) drive->onSynced(true, now);
  }
}

void ackStop(uint32_t now) {
  wire.drain(*tx);
  const cqtest::Sent* s = wire.last(FT::kStop);
  TEST_ASSERT_NOT_NULL(s);
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::AckMsg{s->seq, 0}, 9, f);
  unoSends(f, n, now);
  pump(now);
}

void sync() {
  uint8_t f[64];
  size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 1, 1, 0x1UL}, 1, f);  // caps: encoders
  unoSends(f, n, 10);
  n = cqtest::frame(proto::TelemFastMsg{}, 2, f);
  unoSends(f, n, 20);
  pump(20);
  wire.drain(*tx);
  wire.clear();
}

void driveFrom(int fd, int v, uint32_t now) {
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "{\"t\":\"drive\",\"v\":%d,\"w\":0}", v);
  if (drive->admitted(fd, core::WsCmdKind::kDrive, -1, ++g_post, now)) drive->onDrive(fd, P(cmd), now);
}

void stopFrom(int fd, int32_t id, uint32_t now) { drive->onStop(fd, id, ++g_post, now); }
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  logbook = core::EventLog();
  delete drive; delete settings; delete motion; delete uno; delete tel; delete owner; delete link; delete tx;
  tx = new core::FrameTx();
  link = new core::BodyLink(*tx);
  owner = new core::LinkOwner();
  tel = new app::TelemetryPublisher(out, *link);
  uno = new app::UnoChannel(*link, *tx, out);
  motion = new app::MotionController(out, logbook, *link, *owner, *tel);
  settings = new app::SettingsController(out, logbook, *link);
  drive = new app::DriveController(out, logbook, *owner, *uno, *motion, *settings, *tel);
  motion->setHooks({[](int fd, uint32_t now) { return drive->claim(fd, now); }, [] { return drive->otaActive(); }});
  tel->setViews({[] { return drive->ownerView(); }, [] { return motion->view(); }, [] { return !link->synced(); }});
  motion->begin();
  wire.clear();
  g_post = 0;
  drive->begin(0);
  drive->onClientOpen(3, 0);
  drive->onClientOpen(4, 0);
}
void tearDown() {}

void test_drive_before_sync_is_body_offline() {
  driveFrom(3, 50, 100);
  TEST_ASSERT_TRUE(has(3, "BODY_OFFLINE"));
}

void test_first_drive_takes_control_and_reaches_the_uno() {
  sync();
  driveFrom(3, 50, 100);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kDrive));
  TEST_ASSERT_EQUAL(3, owner->driverFd());
}

void test_second_phone_gets_not_owner_once_per_stream() {
  sync();
  driveFrom(3, 50, 100);
  driveFrom(4, 50, 110);
  driveFrom(4, 50, 120);
  TEST_ASSERT_EQUAL(1, fakes::countTo(4, "NOT_OWNER"));
}

void test_stop_from_anyone_is_sent_and_answered() {
  sync();
  driveFrom(3, 50, 100);
  stopFrom(4, 7, 110);
  ackStop(120);
  TEST_ASSERT_TRUE(has(4, "\"t\":\"ack\",\"id\":7"));
}

void test_stop_is_never_rate_limited() {
  sync();
  for (int i = 0; i < 60; ++i) stopFrom(3, i, 100 + static_cast<uint32_t>(i));
  TEST_ASSERT_FALSE(has(3, "RATE_LIMITED"));
  wire.drain(*tx);
  TEST_ASSERT_TRUE(wire.count(FT::kStop) >= 1);
}

void test_motion_posted_before_a_stop_is_dropped() {
  sync();
  const uint32_t before = ++g_post;  // a drive posted ...
  stopFrom(3, -1, 100);              // ... then a STOP handled first
  TEST_ASSERT_FALSE(drive->admitted(3, core::WsCmdKind::kDrive, -1, before, 101));
  TEST_ASSERT_TRUE(drive->admitted(3, core::WsCmdKind::kDrive, -1, ++g_post, 102));
}

void test_stop_from_non_driver_holds_until_stick_released() {  // design-notes §72
  sync();
  driveFrom(3, 50, 100);
  stopFrom(4, -1, 110);
  wire.drain(*tx);
  wire.clear();
  driveFrom(3, 50, 120);
  TEST_ASSERT_TRUE(has(3, "STOPPED"));
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(0, wire.count(FT::kDrive));
  driveFrom(3, 0, 130);  // stick released
  driveFrom(3, 50, 140);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(2, wire.count(FT::kDrive));
}

void test_stop_hold_is_logged_with_the_stoppers_name() {
  sync();
  driveFrom(3, 50, 100);
  stopFrom(4, -1, 110);
  core::LogEntry e[4];
  bool gap = false;
  const uint8_t n = logbook.since(0, e, 4, gap);
  bool found = false;
  for (uint8_t i = 0; i < n; ++i) found = found || (e[i].kind == core::LogKind::kStopHold && strcmp(e[i].client, ws.name(4)) == 0);
  TEST_ASSERT_TRUE(found);
}

void test_handback_after_5_s_sends_stop() {
  sync();
  driveFrom(3, 50, 100);
  wire.drain(*tx);
  wire.clear();
  drive->tick(5099);
  TEST_ASSERT_EQUAL(3, owner->driverFd());
  drive->tick(5100);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kStop));
  TEST_ASSERT_EQUAL(core::LinkOwner::kNoFd, owner->driverFd());
}

void test_driver_close_mid_move() {  // Review Focus 3
  sync();
  motion->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(motion->active());
  wire.drain(*tx);
  wire.clear();
  drive->onClientClose(3, 200);
  wire.drain(*tx);
  TEST_ASSERT_TRUE(wire.count(FT::kStop) >= 1);
  TEST_ASSERT_FALSE(motion->active());
  TEST_ASSERT_TRUE(has(4, "primitive_done"));
  TEST_ASSERT_FALSE(has(4, "nack"));
  TEST_ASSERT_TRUE(link->synced());  // R14: the master stays
  core::LogEntry e[8];
  bool gap = false;
  const uint8_t n = logbook.since(0, e, 8, gap);
  bool lost = false;
  for (uint8_t i = 0; i < n; ++i) lost = lost || e[i].kind == core::LogKind::kClientLost;
  TEST_ASSERT_TRUE(lost);
}

void test_release_ends_a_primitive() {
  sync();
  motion->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  drive->onRelease(3, 2, 150);
  TEST_ASSERT_FALSE(motion->active());
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":2"));
}

void test_ir_takeover_drops_the_driver() {
  sync();
  driveFrom(3, 50, 100);
  drive->onUnoOwner(static_cast<uint8_t>(proto::Owner::kIr), 110);
  TEST_ASSERT_EQUAL(core::LinkOwner::kNoFd, owner->driverFd());
}

void test_ota_refuses_drive_busy_and_closes_clients() {
  sync();
  drive->startOta(100);
  driveFrom(3, 50, 110);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
  TEST_ASSERT_EQUAL(2, fakes::world().closed.size());
  TEST_ASSERT_FALSE(link->synced());
}

void test_cam_home_waits_in_the_queue() {
  sync();
  drive->onUnoRequest(3, 5, P("{\"t\":\"cam_home\",\"id\":5}"));
  TEST_ASSERT_FALSE(has(3, "nack"));
  TEST_ASSERT_FALSE(uno->empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_drive_before_sync_is_body_offline);
  RUN_TEST(test_first_drive_takes_control_and_reaches_the_uno);
  RUN_TEST(test_second_phone_gets_not_owner_once_per_stream);
  RUN_TEST(test_stop_from_anyone_is_sent_and_answered);
  RUN_TEST(test_stop_is_never_rate_limited);
  RUN_TEST(test_motion_posted_before_a_stop_is_dropped);
  RUN_TEST(test_stop_from_non_driver_holds_until_stick_released);
  RUN_TEST(test_stop_hold_is_logged_with_the_stoppers_name);
  RUN_TEST(test_handback_after_5_s_sends_stop);
  RUN_TEST(test_driver_close_mid_move);
  RUN_TEST(test_release_ends_a_primitive);
  RUN_TEST(test_ir_takeover_drops_the_driver);
  RUN_TEST(test_ota_refuses_drive_busy_and_closes_clients);
  RUN_TEST(test_cam_home_waits_in_the_queue);
  return UNITY_END();
}
