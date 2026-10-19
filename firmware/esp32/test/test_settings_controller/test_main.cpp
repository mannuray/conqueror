// SettingsController: WiFi / calibration / settings changes, the tank's button, restart (bridge LLD R9, R13, R15.4).
#include <string.h>
#include <unity.h>

#include <vector>

#include "app/settings_controller.h"
#include "core/motion_config.h"
#include "core/wifi_policy.h"
#include "core/ws_codec.h"
#include "fakes/fake_ports.h"

using namespace cq;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::EventLog logbook;
core::FrameTx tx;
core::BodyLink link(tx);
app::SettingsController* sc = nullptr;

bool g_active = false, g_ota = false;
int g_driver = -1;
int g_calib_sent = 0;
std::vector<core::AckedOp> g_ops;
size_t g_free = 8;  // free request slots the fake queue reports
uint16_t g_cpm = 7514, g_roll = 27;

core::WsCommand P(const char* s) { return core::parseWsCommand(s, strlen(s)); }
bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  ws.add(4, 0);
  logbook = core::EventLog();
  delete sc;
  sc = new app::SettingsController(out, logbook, link);  // host test: a fresh controller per test
  g_active = g_ota = false;
  g_driver = -1;
  g_calib_sent = 0;
  g_ops.clear();
  g_free = 8;
  g_cpm = 7514;
  g_roll = 27;
  sc->setHooks({[] { return g_active; }, [] { return g_ota; }, [] { return g_driver; },
                     [](int) { ++g_calib_sent; },
                     [](const core::AckedOp& op) {
                       if (g_free == 0) return false;
                       --g_free;
                       g_ops.push_back(op);
                       return true;
                     },
                     &g_cpm, &g_roll, [] { return g_free; }});
}
void tearDown() {}

void test_change_waits_for_the_button_then_posts_wifi_with_feedback() {
  sc->onCommand(3, 1, P("{\"t\":\"wifi_mode\",\"mode\":\"ap\",\"id\":1}"), 1000);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"confirm\""));
  TEST_ASSERT_EQUAL(0, fakes::world().wifi_posted.size());
  sc->onButtonPress(2000);
  TEST_ASSERT_EQUAL(1, fakes::world().wifi_posted.size());
  TEST_ASSERT_TRUE(fakes::world().wifi_posted[0].ap);
  TEST_ASSERT_EQUAL(2, g_ops.size());  // jingle + white blink
  TEST_ASSERT_EQUAL(core::AckedOp::kSound, g_ops[0].kind);
}

void test_double_press_toggles_wifi_with_feedback() {
  strcpy(fakes::world().wifi.mode, "home");
  sc->onButtonPress(1000);
  TEST_ASSERT_EQUAL(0, fakes::world().wifi_posted.size());
  sc->onButtonPress(1500);
  TEST_ASSERT_EQUAL(1, fakes::world().wifi_posted.size());
  TEST_ASSERT_TRUE(fakes::world().wifi_posted[0].ap);
  TEST_ASSERT_EQUAL(2, g_ops.size());
}

void test_another_devices_change_is_refused_busy() {
  sc->onCommand(3, 1, P("{\"t\":\"wifi_forget\",\"id\":1}"), 1000);
  sc->onCommand(4, 2, P("{\"t\":\"wifi_mode\",\"mode\":\"home\",\"id\":2}"), 1100);
  TEST_ASSERT_TRUE(has(4, "BUSY"));
}

void test_setting_change_waits_the_full_window() {  // the window starts when the change is asked
  sc->onCommand(3, 5, P("{\"t\":\"setting\",\"setting_id\":66,\"value\":19,\"id\":5}"), 100000);
  sc->tick(100000 + 29000);
  TEST_ASSERT_FALSE(has(3, "confirm_expired"));
  sc->tick(100000 + 30000);
  TEST_ASSERT_TRUE(has(3, "confirm_expired"));
}

void test_setting_write_only_track_balance_in_range() {
  sc->onCommand(3, 6, P("{\"t\":\"setting\",\"setting_id\":11,\"value\":5,\"id\":6}"), 1000);
  TEST_ASSERT_TRUE(has(3, "INVALID_COMMAND"));
  sc->onCommand(3, 7, P("{\"t\":\"setting\",\"setting_id\":66,\"value\":101,\"id\":7}"), 1000);
  TEST_ASSERT_EQUAL(2, fakes::countTo(3, "INVALID_COMMAND"));
}

void test_setting_confirmed_queues_set_save_get() {
  sc->onCommand(3, 8, P("{\"t\":\"setting\",\"setting_id\":66,\"value\":19,\"id\":8}"), 1000);
  sc->onButtonPress(2000);
  TEST_ASSERT_EQUAL(3, g_ops.size());
  TEST_ASSERT_EQUAL(core::AckedOp::kSettingGet, g_ops[2].kind);
  TEST_ASSERT_EQUAL(3, g_ops[2].fd);
}

void test_setting_not_applied_half_when_the_queue_is_nearly_full() {  // final review I1
  sc->onCommand(3, 11, P("{\"t\":\"setting\",\"setting_id\":66,\"value\":19,\"id\":11}"), 1000);
  g_free = 2;  // phones' sound / camera requests fill the shared queue
  sc->onButtonPress(2000);
  TEST_ASSERT_EQUAL(0, g_ops.size());  // nothing half done
  TEST_ASSERT_TRUE(has(3, "BUSY"));
  TEST_ASSERT_FALSE(has(3, "\"t\":\"confirmed\""));
}

void test_calib_set_only_for_the_driver_and_not_while_moving() {
  sc->onCalib(3, 1, P("{\"t\":\"calib\",\"counts_per_m\":7300,\"id\":1}"), 1000);
  TEST_ASSERT_TRUE(has(3, "NOT_OWNER"));
  g_driver = 3;
  g_active = true;
  sc->onCalib(3, 2, P("{\"t\":\"calib\",\"counts_per_m\":7300,\"id\":2}"), 1000);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
  g_active = false;
  sc->onCalib(3, 3, P("{\"t\":\"calib\",\"counts_per_m\":7300,\"id\":3}"), 1000);
  sc->onButtonPress(2000);
  TEST_ASSERT_EQUAL(7300, g_cpm);
  TEST_ASSERT_EQUAL(1, g_calib_sent);
}

void test_calib_save_failure_is_busy() {
  g_driver = 3;
  fakes::world().save_ok = false;
  sc->onCalib(3, 4, P("{\"t\":\"calib\",\"counts_per_m\":7300,\"id\":4}"), 1000);
  sc->onButtonPress(2000);
  TEST_ASSERT_EQUAL(7514, g_cpm);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
}

void test_calib_read_sends_calib() {
  sc->onCalib(3, -1, P("{\"t\":\"calib\"}"), 1000);
  TEST_ASSERT_EQUAL(1, g_calib_sent);
}

void test_wifi_mode_result_is_logged() {
  core::WifiResult r;
  r.kind = core::WifiResult::kModeChanged;
  r.id = static_cast<int32_t>(core::WifiMode::kForcedAp);
  fakes::world().wifi_results.push_back(r);
  sc->tick(1000);
  TEST_ASSERT_EQUAL(1, logbook.last());
}

void test_restart_is_acked_logged_and_due_after_500_ms() {
  sc->onCommand(3, 9, P("{\"t\":\"restart\",\"id\":9}"), 1000);
  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":9"));
  sc->tick(1499);
  TEST_ASSERT_EQUAL(0, fakes::world().restarts);
  sc->tick(1500);
  TEST_ASSERT_EQUAL(1, fakes::world().restarts);
}

void test_restart_refused_while_moving() {
  g_active = true;
  sc->onCommand(3, 10, P("{\"t\":\"restart\",\"id\":10}"), 1000);
  TEST_ASSERT_TRUE(has(3, "BUSY"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_change_waits_for_the_button_then_posts_wifi_with_feedback);
  RUN_TEST(test_double_press_toggles_wifi_with_feedback);
  RUN_TEST(test_another_devices_change_is_refused_busy);
  RUN_TEST(test_setting_change_waits_the_full_window);
  RUN_TEST(test_setting_write_only_track_balance_in_range);
  RUN_TEST(test_setting_confirmed_queues_set_save_get);
  RUN_TEST(test_setting_not_applied_half_when_the_queue_is_nearly_full);
  RUN_TEST(test_calib_set_only_for_the_driver_and_not_while_moving);
  RUN_TEST(test_calib_save_failure_is_busy);
  RUN_TEST(test_calib_read_sends_calib);
  RUN_TEST(test_wifi_mode_result_is_logged);
  RUN_TEST(test_restart_is_acked_logged_and_due_after_500_ms);
  RUN_TEST(test_restart_refused_while_moving);
  return UNITY_END();
}
