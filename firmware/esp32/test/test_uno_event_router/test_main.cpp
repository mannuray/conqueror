// UnoEventRouter: Uno events and link changes -> pages, Log, controllers (bridge LLD R13, R15.4).
#include <unity.h>

#include "app/uno_event_router.h"
#include "fakes/fake_ports.h"

using namespace cq;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::EventLog logbook;
core::FrameTx tx;
core::BodyLink link(tx);
app::TelemetryPublisher tel(out, link);
int g_online = 0, g_offline = 0, g_owner = -1, g_button = 0, g_motion = -1, g_ack = -1, g_stop = -1, g_value = -1;

app::UnoEventRouter::Sinks sinks() {
  app::UnoEventRouter::Sinks s{};
  s.body_online = [](uint32_t) { ++g_online; };
  s.body_offline = [](uint8_t, int16_t, uint32_t) { ++g_offline; };
  s.uno_owner = [](uint8_t o, uint32_t) { g_owner = o; };
  s.button_press = [](uint32_t) { ++g_button; };
  s.motion_event = [](uint8_t e, uint32_t) { g_motion = e; };
  s.ack_result = [](uint8_t seq, uint8_t) { g_ack = seq; };
  s.stop_result = [](uint8_t st) { g_stop = st; };
  s.setting_value = [](uint8_t, int32_t v) { g_value = v; };
  return s;
}
app::UnoEventRouter router(out, logbook, tel, link);

core::LinkEvent unoEvent(proto::EventKind k, int16_t a = 0) {
  return {core::LinkEventKind::kUnoEvent, static_cast<uint8_t>(k), 0, 0, a, 0};
}
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  logbook = core::EventLog();
  g_online = g_offline = g_button = 0;
  g_owner = g_motion = g_ack = g_stop = g_value = -1;
  router.setSinks(sinks());
}
void tearDown() {}

void test_uno_event_broadcast_and_logged_except_button_and_unblocked() {
  router.route(unoEvent(proto::EventKind::kStall, 1), 0);
  router.route(unoEvent(proto::EventKind::kUnblocked), 0);
  router.route(unoEvent(proto::EventKind::kButtonPress), 0);
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "\"kind\":\"stall\""));
  TEST_ASSERT_EQUAL(1, logbook.last());  // only the stall
}

void test_button_press_goes_to_the_button_sink() {
  router.route(unoEvent(proto::EventKind::kButtonPress), 0);
  TEST_ASSERT_EQUAL(1, g_button);
}

void test_stall_and_encoder_fault_reach_motion() {
  router.route(unoEvent(proto::EventKind::kStall), 0);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kStall), g_motion);
  router.route(unoEvent(proto::EventKind::kEncoderFault), 0);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kEncoderFault), g_motion);
}

void test_control_changed_reaches_owner_sink() {
  router.route(unoEvent(proto::EventKind::kControlChanged, static_cast<int16_t>(proto::Owner::kIr)), 0);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), g_owner);
}

void test_body_offline_broadcast_logged_and_sunk() {
  router.route({core::LinkEventKind::kBodyOffline, 0, 0, 0, 0, 0}, 0);
  TEST_ASSERT_EQUAL(1, g_offline);
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "body_offline"));
  TEST_ASSERT_EQUAL(1, logbook.last());
}

void test_body_online_broadcast_logged_and_sunk() {
  router.route({core::LinkEventKind::kBodyOnline, 0, 0, 0, 0, 0}, 0);
  TEST_ASSERT_EQUAL(1, g_online);
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "body_online"));
  TEST_ASSERT_EQUAL(1, logbook.last());
  TEST_ASSERT_EQUAL(1, fakes::world().logs.size());  // "uno: synced fw ..."
}

void test_answers_reach_their_sinks() {
  router.route({core::LinkEventKind::kAckResult, 0, 9, 0, 0, 0}, 0);
  router.route({core::LinkEventKind::kStopResult, 0, 4, 0, 0, 0}, 0);
  router.route({core::LinkEventKind::kSettingValue, 66, 0, 0, 19, 0}, 0);
  TEST_ASSERT_EQUAL(9, g_ack);
  TEST_ASSERT_EQUAL(0, g_stop);
  TEST_ASSERT_EQUAL(19, g_value);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_uno_event_broadcast_and_logged_except_button_and_unblocked);
  RUN_TEST(test_button_press_goes_to_the_button_sink);
  RUN_TEST(test_stall_and_encoder_fault_reach_motion);
  RUN_TEST(test_control_changed_reaches_owner_sink);
  RUN_TEST(test_body_offline_broadcast_logged_and_sunk);
  RUN_TEST(test_body_online_broadcast_logged_and_sunk);
  RUN_TEST(test_answers_reach_their_sinks);
  return UNITY_END();
}
