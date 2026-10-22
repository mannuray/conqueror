// MotionController: turn / move / drive_rate / cancel, primitive ends, counts per metre, the learned turn coast (bridge LLD R4, R7, R8, R15.4, R18).
#include <string.h>
#include <unity.h>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/motion_controller.h"
#include "core/arc_limits.h"
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
core::LinkOwner owner;
app::TelemetryPublisher* tel = nullptr;
app::MotionController* mc = nullptr;
cqtest::Wire wire;
core::Decision g_claim = core::Decision::kSend;
bool g_ota = false;

core::WsCommand P(const char* s) { return core::parseWsCommand(s, strlen(s)); }
bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }

void unoSends(const uint8_t* f, size_t n, uint32_t now) { link->onUartBytes(f, n, now); }

void syncedWithEncoders(uint32_t flags = 0) {
  link->startMaster(0);
  owner.begin();
  owner.onWsOpen(3);
  owner.onWsOpen(4);
  owner.onSynced(true);
  uint8_t f[64];
  size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 1, 1, 0x1UL}, 1, f);  // caps: encoders only
  unoSends(f, n, 10);
  proto::TelemFastMsg t{};
  t.flags = flags;
  n = cqtest::frame(t, 2, f);
  unoSends(f, n, 20);
  wire.drain(*tx);
  wire.clear();
}
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  ws.add(4, 0);
  logbook = core::EventLog();
  owner = core::LinkOwner();
  delete mc;
  delete tel;
  delete link;
  delete tx;
  tx = new core::FrameTx();
  link = new core::BodyLink(*tx);
  tel = new app::TelemetryPublisher(out, *link);
  mc = new app::MotionController(out, logbook, *link, owner, *tel);
  g_claim = core::Decision::kSend;
  g_ota = false;
  mc->setHooks({[](int fd, uint32_t now) {
                  if (g_claim != core::Decision::kSend) return g_claim;
                  core::Actions a;
                  return owner.onDrive(fd, now, a);  // the real ownership rules
                },
                [] { return g_ota; }});
  mc->begin();
  wire.clear();
}
void tearDown() {}

void test_begin_loads_counts_and_roll_on() {
  TEST_ASSERT_EQUAL(7514, mc->cpm());
  TEST_ASSERT_EQUAL(27, mc->rollMm());
}

void test_turn_needs_a_synced_body() {
  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(has(3, "BODY_OFFLINE"));
}

void test_turn_needs_ownership_and_acks() {
  syncedWithEncoders();
  g_claim = core::Decision::kNackNotOwner;
  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(has(3, "NOT_OWNER"));
  g_claim = core::Decision::kSend;
  mc->onTurn(3, 2, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":2}"), 100);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":2"));
  TEST_ASSERT_TRUE(mc->active());
  TEST_ASSERT_TRUE(mc->ownedBy(3));
}

void test_turn_refused_busy_during_ota() {
  syncedWithEncoders();
  g_ota = true;
  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
}

void test_arc_outside_its_speed_band_is_invalid_with_a_reason() {
  syncedWithEncoders();
  uint16_t radius = 0;  // a radius whose speed band does not include 40 %
  for (uint16_t r = 1; r <= 1000 && radius == 0; ++r) {
    const core::ArcBand b = core::arcBand(r);
    if (!b.possible || 40 < b.lo || 40 > b.hi) radius = r;
  }
  TEST_ASSERT_NOT_EQUAL(0, radius);
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "{\"t\":\"turn\",\"by\":90,\"radius_cm\":%u,\"speed\":40,\"id\":1}", radius);
  mc->onTurn(3, 1, P(cmd), 100);
  TEST_ASSERT_TRUE(has(3, "INVALID_COMMAND"));
  TEST_ASSERT_FALSE(mc->active());
}

void test_move_refused_with_an_encoder_fault() {
  syncedWithEncoders(1u << static_cast<unsigned>(proto::FastFlagBit::kEncoderFault));
  mc->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(has(3, "UNAVAILABLE"));
}

void test_move_ended_by_stop_is_broadcast_and_logged_without_a_second_stop() {
  syncedWithEncoders();
  mc->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(mc->active());
  mc->end(core::MotionEvent::kStop, 200);  // the STOP handler already sent STOP
  TEST_ASSERT_FALSE(mc->active());
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(0, wire.count(FT::kStop));
  TEST_ASSERT_TRUE(has(4, "primitive_done"));  // every client
  TEST_ASSERT_EQUAL(1, logbook.last());       // ended early
}

void test_cancelled_move_sends_stop() {
  syncedWithEncoders();
  mc->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  mc->end(core::MotionEvent::kCancel, 200);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kStop));
}

void test_cancel_by_another_client_is_not_owner() {
  syncedWithEncoders();
  mc->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  mc->onCancel(4, 2, 150);
  TEST_ASSERT_TRUE(has(4, "NOT_OWNER"));
  TEST_ASSERT_TRUE(mc->active());
  mc->onCancel(3, 3, 160);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":3"));
  TEST_ASSERT_FALSE(mc->active());
}

// R18.2: a done turn reports its coast reading; the controller averages it in and saves it.
namespace {
void yawFrame(uint16_t yaw, int16_t rate, uint32_t now) {
  proto::TelemFastMsg f{};
  f.yaw = yaw;
  f.yaw_rate = rate;
  uint8_t b[64];
  const size_t n = cqtest::frame(f, 3, b);
  unoSends(b, n, now);
  mc->onTelemetry(now);
}
}  // namespace

void test_a_done_turn_saves_the_learned_coast() {
  TEST_ASSERT_EQUAL(60, mc->coastMs());  // loaded at begin
  syncedWithEncoders();
  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
  TEST_ASSERT_TRUE(mc->active());
  uint32_t t = 100;
  yawFrame(8700, 650, t += 20);  // cut at 87° turning 65 °/s
  for (int i = 0; i < 30 && mc->active(); ++i) yawFrame(8990, 0, t += 20);  // glided 2.9°, stopped
  TEST_ASSERT_FALSE(mc->active());
  TEST_ASSERT_TRUE(has(3, "primitive_done"));
  TEST_ASSERT_EQUAL(1, static_cast<int>(fakes::world().saved_coast.size()));
  TEST_ASSERT_EQUAL(56, fakes::world().saved_coast[0]);  // (3 x 60 + 44 + 2) / 4
  TEST_ASSERT_EQUAL(56, mc->coastMs());
}

void test_a_cancelled_turn_learns_nothing() {
  syncedWithEncoders();
  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
  yawFrame(8700, 650, 120);
  mc->end(core::MotionEvent::kCancel, 140);
  TEST_ASSERT_EQUAL(0, static_cast<int>(fakes::world().saved_coast.size()));
  TEST_ASSERT_EQUAL(60, mc->coastMs());
}

// R19: the saved gyro scale reaches the heading correction at begin, and the calib reply shows it.
void test_begin_loads_the_gyro_scale_and_calib_shows_it() {
  fakes::world().gyro = 10230;
  mc->begin();
  TEST_ASSERT_EQUAL(10230, link->gyroScale());
  mc->sendCalib(3);
  TEST_ASSERT_TRUE(has(3, "\"gyro_scale\":10230"));
  TEST_ASSERT_TRUE(mc->setGyroScale(10100));
  TEST_ASSERT_EQUAL(10100, link->gyroScale());
  TEST_ASSERT_FALSE(mc->setGyroScale(20000));
}

// Tank Logo LLD §22 / bridge LLD R20: a Logo GO starts the GO primitive (DRIVE frames at its speed), the program
// keeps it alive, HOLD ends it with a STOP; GO needs no encoders.
void test_logo_go_and_hold() {
  syncedWithEncoders();
  core::Actions took;
  owner.onDrive(3, 50, took);  // logo_run takes the drive first (Tank Logo LLD §20.5)
  logo::Action go;
  go.kind = logo::ActKind::kGo;
  go.speed_pct = 40;
  core::BridgeReason why = core::BridgeReason::kBusy;
  TEST_ASSERT_TRUE(mc->startLogo(3, go, 100, why));
  TEST_ASSERT_EQUAL(static_cast<int>(core::PrimKind::kGo), static_cast<int>(mc->view().kind));
  mc->keepLogoGo(350);
  mc->onTelemetry(360);  // 260 ms since the start, 10 since the keep: still going
  TEST_ASSERT_TRUE(mc->active());
  wire.drain(*tx);
  wire.clear();
  logo::Action hold;
  hold.kind = logo::ActKind::kHold;
  TEST_ASSERT_TRUE(mc->startLogo(3, hold, 400, why));
  TEST_ASSERT_FALSE(mc->active());
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kStop));
  TEST_ASSERT_TRUE(has(4, "\"prim\":\"go\",\"ok\":true,\"reason\":\"done\""));  // to every page
}

void test_calib_reply_has_counts_and_roll_diagnostics() {
  mc->sendCalib(3);
  const std::string j = fakes::lastTo(3);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"counts_per_m\":7514"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"roll_watched\":0"));
}

// ---- Plan N3: a running Logo program's motions and the events that end it ----
int g_logo_fd = -1;
int g_logo_ends = 0;
cq::core::EndReason g_logo_reason = cq::core::EndReason::kDone;
int g_logo_events = 0;
cq::core::MotionEvent g_logo_event = cq::core::MotionEvent::kStop;

void logoHooks() {
  app::MotionController::Hooks h;
  h.claim = [](int fd, uint32_t now) {
    core::Actions a;
    return owner.onDrive(fd, now, a);
  };
  h.ota = [] { return g_ota; };
  h.logo_fd = [] { return g_logo_fd; };
  h.logo_end = [](const core::PrimitiveEnd& e, uint32_t) {
    ++g_logo_ends;
    g_logo_reason = e.reason;
  };
  h.logo_event = [](core::MotionEvent e, uint32_t) {
    ++g_logo_events;
    g_logo_event = e;
  };
  mc->setHooks(h);
  g_logo_fd = -1;
  g_logo_ends = g_logo_events = 0;
}

logo::Action logoMove(int16_t cm) {
  logo::Action a;
  a.kind = logo::ActKind::kMove;
  a.dist_cm = cm;
  a.speed_pct = 40;
  return a;
}

void test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program() {
  syncedWithEncoders();
  logoHooks();
  core::BridgeReason why = core::BridgeReason::kBusy;
  TEST_ASSERT_TRUE(mc->startLogo(3, logoMove(50), 100, why));
  TEST_ASSERT_TRUE(mc->active());
  TEST_ASSERT_EQUAL(0, fakes::countTo(3, "ack"));
  mc->end(core::MotionEvent::kCancel, 200);
  TEST_ASSERT_EQUAL(1, g_logo_ends);
  TEST_ASSERT_EQUAL(core::EndReason::kCancelled, g_logo_reason);
  TEST_ASSERT_EQUAL(1, g_logo_events);
  TEST_ASSERT_EQUAL(core::MotionEvent::kCancel, g_logo_event);
}

void test_a_page_s_own_primitive_end_does_not_reach_the_program() {
  syncedWithEncoders();
  logoHooks();
  mc->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
  mc->end(core::MotionEvent::kCancel, 200);
  TEST_ASSERT_EQUAL(0, g_logo_ends);
}

void test_logo_motions_are_refused_like_the_page_s() {
  logoHooks();
  core::BridgeReason why = core::BridgeReason::kBusy;
  TEST_ASSERT_FALSE(mc->startLogo(3, logoMove(50), 100, why));
  TEST_ASSERT_EQUAL(core::BridgeReason::kBodyOffline, why);
  syncedWithEncoders(1u << static_cast<unsigned>(proto::FastFlagBit::kEncoderFault));
  TEST_ASSERT_FALSE(mc->startLogo(3, logoMove(50), 100, why));
  TEST_ASSERT_EQUAL(core::BridgeReason::kUnavailable, why);
  logo::Action turn;
  turn.kind = logo::ActKind::kTurn;
  turn.delta_cdeg = -9000;
  turn.speed_pct = 10;  // below the slowest spin: started at 12 %
  TEST_ASSERT_TRUE(mc->startLogo(3, turn, 100, why));  // a spin needs no encoders
  g_ota = true;
  TEST_ASSERT_FALSE(mc->startLogo(3, turn, 100, why));
  TEST_ASSERT_EQUAL(core::BridgeReason::kBusy, why);
}

void test_a_running_program_counts_as_its_page_s_primitive() {
  logoHooks();
  TEST_ASSERT_FALSE(mc->ownedBy(3));
  g_logo_fd = 3;
  TEST_ASSERT_TRUE(mc->ownedBy(3));  // so release / page closed / owner command reach it (end -> logo_event)
  TEST_ASSERT_FALSE(mc->ownedBy(4));
  mc->end(core::MotionEvent::kOwnerClosed, 100);
  TEST_ASSERT_EQUAL(core::MotionEvent::kOwnerClosed, g_logo_event);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_begin_loads_counts_and_roll_on);
  RUN_TEST(test_turn_needs_a_synced_body);
  RUN_TEST(test_turn_needs_ownership_and_acks);
  RUN_TEST(test_turn_refused_busy_during_ota);
  RUN_TEST(test_arc_outside_its_speed_band_is_invalid_with_a_reason);
  RUN_TEST(test_move_refused_with_an_encoder_fault);
  RUN_TEST(test_move_ended_by_stop_is_broadcast_and_logged_without_a_second_stop);
  RUN_TEST(test_cancelled_move_sends_stop);
  RUN_TEST(test_cancel_by_another_client_is_not_owner);
  RUN_TEST(test_a_done_turn_saves_the_learned_coast);
  RUN_TEST(test_a_cancelled_turn_learns_nothing);
  RUN_TEST(test_begin_loads_the_gyro_scale_and_calib_shows_it);
  RUN_TEST(test_logo_go_and_hold);
  RUN_TEST(test_calib_reply_has_counts_and_roll_diagnostics);
  RUN_TEST(test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program);
  RUN_TEST(test_a_page_s_own_primitive_end_does_not_reach_the_program);
  RUN_TEST(test_logo_motions_are_refused_like_the_page_s);
  RUN_TEST(test_a_running_program_counts_as_its_page_s_primitive);
  return UNITY_END();
}
