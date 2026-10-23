// TankAppController: tank / log / camera_set / selftest / app_set (bridge LLD R13, R15.4).
#include <string.h>
#include <unity.h>

#include "app/tank_app_controller.h"
#include "core/ws_codec.h"
#include "fakes/fake_ports.h"

using namespace cq;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::EventLog logbook;
core::FrameTx tx;
core::BodyLink link(tx);
app::TankAppController tank(out, logbook, link);

bool g_offline = false, g_moving = false, g_queue_ok = true;
int g_queued = 0;
bool offline() { return g_offline; }
bool moving() { return g_moving; }
bool queue(int, int32_t) {
  ++g_queued;
  return g_queue_ok;
}

core::WsCommand P(const char* s) { return core::parseWsCommand(s, strlen(s)); }
bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  logbook = core::EventLog();
  g_offline = g_moving = false;
  g_queue_ok = true;
  g_queued = 0;
  tank.setSelfTestHooks({offline, moving, queue});
  fakes::world().ai_url = "http://192.168.1.20:8000/";
  tank.begin();
}
void tearDown() {}

void test_tank_reply_has_camera_and_ai_url() {
  fakes::world().cam.size = 3;
  fakes::world().viewers = 2;
  tank.onTank(3, 1000);
  const std::string j = fakes::lastTo(3);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"size\":\"HD\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"viewers\":2"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"ai_url\":\"http://192.168.1.20:8000/\""));
}

void test_log_pages_twenty_five_at_a_time() {
  for (int i = 0; i < 35; ++i) logbook.add(core::LogKind::kRestart, 0, 0, "x", 0, 0);
  tank.onLog(3, 0, 1000);
  const std::string j = fakes::lastTo(3);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"seq\":25,"));
  TEST_ASSERT_EQUAL(std::string::npos, j.find("\"seq\":26,"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"last\":35"));
}

void test_camera_set_merges_only_sent_fields_then_acks_and_sends_tank() {
  fakes::world().cam.quality = 20;
  fakes::world().cam.vflip = true;
  tank.onCameraSet(3, 5, P("{\"t\":\"camera_set\",\"vflip\":false,\"id\":5}"), 1000);
  TEST_ASSERT_EQUAL(1, fakes::world().cam_posted.size());
  TEST_ASSERT_FALSE(fakes::world().cam_posted[0].vflip);
  TEST_ASSERT_EQUAL(20, fakes::world().cam_posted[0].quality);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":5"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, fakes::lastTo(3).find("\"vflip\":false"));  // the posted settings
}

void test_camera_set_white_balance_and_night_are_merged_and_reported() {  // R24
  tank.onCameraSet(3, 9, P("{\"t\":\"camera_set\",\"wb\":\"office\",\"night\":true,\"id\":9}"), 1000);
  TEST_ASSERT_EQUAL(1, fakes::world().cam_posted.size());
  TEST_ASSERT_EQUAL(3, fakes::world().cam_posted[0].wb);
  TEST_ASSERT_TRUE(fakes::world().cam_posted[0].night);
  const std::string j = fakes::lastTo(3);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"wb\":\"office\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"night\":true"));
}

void test_camera_set_reset_uses_the_defaults() {
  fakes::world().cam.quality = 30;
  fakes::world().cam_defaults.quality = 12;
  tank.onCameraSet(3, 6, P("{\"t\":\"camera_set\",\"reset\":true,\"id\":6}"), 1000);
  TEST_ASSERT_EQUAL(12, fakes::world().cam_posted[0].quality);
}

void test_camera_set_without_camera_is_unavailable() {
  fakes::world().cam_ok = false;
  tank.onCameraSet(3, 7, P("{\"t\":\"camera_set\",\"quality\":15,\"id\":7}"), 1000);
  TEST_ASSERT_TRUE(has(3, "UNAVAILABLE"));
}

void test_app_set_saves_and_acks() {
  tank.onAppSet(3, 8, P("{\"t\":\"app_set\",\"ai_url\":\"http://10.0.0.2/\",\"id\":8}"));
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.2/", fakes::world().ai_url.c_str());
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.2/", tank.aiUrl());
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":8"));
}

void test_app_set_save_failure_is_busy() {
  fakes::world().save_ok = false;
  tank.onAppSet(3, 9, P("{\"t\":\"app_set\",\"ai_url\":\"http://10.0.0.2/\",\"id\":9}"));
  TEST_ASSERT_TRUE(has(3, "BUSY"));
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8000/", tank.aiUrl());
}

void test_selftest_refused_while_moving_and_when_offline() {
  g_moving = true;
  tank.onSelfTest(3, 10);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
  g_moving = false;
  g_offline = true;
  tank.onSelfTest(3, 11);
  TEST_ASSERT_TRUE(has(3, "BODY_OFFLINE"));
  TEST_ASSERT_EQUAL(0, g_queued);
}

void test_selftest_queued_or_busy_when_full() {
  tank.onSelfTest(3, 12);
  TEST_ASSERT_EQUAL(1, g_queued);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());  // answered later, when the Uno acks
  g_queue_ok = false;
  tank.onSelfTest(3, 13);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tank_reply_has_camera_and_ai_url);
  RUN_TEST(test_log_pages_twenty_five_at_a_time);
  RUN_TEST(test_camera_set_merges_only_sent_fields_then_acks_and_sends_tank);
  RUN_TEST(test_camera_set_reset_uses_the_defaults);
  RUN_TEST(test_camera_set_white_balance_and_night_are_merged_and_reported);
  RUN_TEST(test_camera_set_without_camera_is_unavailable);
  RUN_TEST(test_app_set_saves_and_acks);
  RUN_TEST(test_app_set_save_failure_is_busy);
  RUN_TEST(test_selftest_refused_while_moving_and_when_offline);
  RUN_TEST(test_selftest_queued_or_busy_when_full);
  return UNITY_END();
}
