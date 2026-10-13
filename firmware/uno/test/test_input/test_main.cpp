#include <unity.h>

#include "domain/input/boot_button.h"
#include "domain/input/ir_codes.h"
#include "domain/input/ir_controller.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_ir_hold_up_drives_then_release_stops() {
  IrController c;
  IrOutput o{};
  TEST_ASSERT_TRUE(c.onEvent({ir_codes::kUp, false, 0}, o));
  TEST_ASSERT_TRUE(o.active);
  TEST_ASSERT_EQUAL(50, o.v);
  TEST_ASSERT_EQUAL(0, o.w);
  TEST_ASSERT_FALSE(c.tick(150, o));
  c.onEvent({ir_codes::kUp, true, 108}, o);  // NEC repeat while held
  TEST_ASSERT_FALSE(c.tick(324, o));         // one repeat lost (216 ms): still held (§39)
  TEST_ASSERT_FALSE(c.tick(457, o));
  TEST_ASSERT_TRUE(c.tick(458, o));          // 350 ms without a repeat → released
  TEST_ASSERT_FALSE(o.active);
  TEST_ASSERT_EQUAL(0, o.v);
}

void test_ir_directions_speed_and_ok() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::k6, false, 0}, o);
  TEST_ASSERT_TRUE(o.speed_changed);
  TEST_ASSERT_EQUAL(60, c.speedPct());
  c.onEvent({ir_codes::k4, false, 10}, o);
  c.onEvent({ir_codes::k4, false, 20}, o);
  TEST_ASSERT_EQUAL(40, c.speedPct());
  c.onEvent({ir_codes::kLeft, false, 30}, o);
  TEST_ASSERT_EQUAL(0, o.v);
  TEST_ASSERT_EQUAL(40, o.w);  // CCW
  c.onEvent({ir_codes::kRight, false, 40}, o);
  TEST_ASSERT_EQUAL(-40, o.w);
  c.onEvent({ir_codes::kDown, false, 50}, o);
  TEST_ASSERT_EQUAL(-40, o.v);
  c.onEvent({ir_codes::kOk, false, 60}, o);
  TEST_ASSERT_TRUE(o.stop);
  TEST_ASSERT_FALSE(o.active);
}

void test_ir_speed_limits() {
  IrController c;
  IrOutput o{};
  for (int i = 0; i < 10; ++i) c.onEvent({ir_codes::k6, false, static_cast<uint32_t>(i * 300)}, o);
  TEST_ASSERT_EQUAL(100, c.speedPct());
  for (int i = 0; i < 10; ++i) c.onEvent({ir_codes::k4, false, static_cast<uint32_t>(3000 + i * 300)}, o);
  TEST_ASSERT_EQUAL(20, c.speedPct());
}

void test_ir_camera_jog_while_held() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::k1, false, 0}, o);
  TEST_ASSERT_EQUAL(-1, o.jog_pan);
  TEST_ASSERT_TRUE(c.tick(350, o));  // kReleaseMs (350) after the last code
  TEST_ASSERT_EQUAL(0, o.jog_pan);
  c.onEvent({ir_codes::k2, false, 400}, o);
  TEST_ASSERT_EQUAL(1, o.jog_tilt);
  c.onEvent({ir_codes::k8, false, 400}, o);
  TEST_ASSERT_EQUAL(-1, o.jog_tilt);
}

void test_ir_one_shot_keys_ignore_repeats() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::k5, false, 0}, o);
  TEST_ASSERT_TRUE(o.cam_home);
  c.onEvent({ir_codes::k5, true, 108}, o);
  TEST_ASSERT_FALSE(o.cam_home);
  c.onEvent({ir_codes::k7, false, 500}, o);
  TEST_ASSERT_TRUE(o.toggle_mute);
  c.onEvent({ir_codes::k9, false, 1000}, o);
  TEST_ASSERT_TRUE(o.battery_check);
  c.onEvent({ir_codes::k0, false, 1500}, o);
  TEST_ASSERT_TRUE(o.turn_180);
  c.onEvent({ir_codes::kStar, false, 2000}, o);
  TEST_ASSERT_FALSE(o.toggle_parking);  // `*` acts on release (a hold may become Safety Override)
  TEST_ASSERT_TRUE(c.tick(2350, o));
  TEST_ASSERT_TRUE(o.toggle_parking);
  TEST_ASSERT_FALSE(c.onEvent({0x12345678UL, false, 2500}, o));
}

void test_ir_hash_hold_3s_requests_calibration_once() {
  IrController c;
  IrOutput o{};
  int requests = 0;
  c.onEvent({ir_codes::kHash, false, 0}, o);
  requests += o.cal_request ? 1 : 0;
  for (uint32_t t = 108; t < 4000; t += 108) {
    c.onEvent({ir_codes::kHash, true, t}, o);
    requests += o.cal_request ? 1 : 0;
  }
  TEST_ASSERT_EQUAL(1, requests);
}

// LLD §10.5: `*` held 2 s toggles Safety Override once; releasing it then is not a parking-beeps tap.
void test_ir_star_hold_2s_toggles_override_once() {
  IrController c;
  IrOutput o{};
  int toggles = 0;
  c.onEvent({ir_codes::kStar, false, 0}, o);
  toggles += o.toggle_override ? 1 : 0;
  for (uint32_t t = 108; t < 3000; t += 108) {
    c.onEvent({ir_codes::kStar, true, t}, o);
    toggles += o.toggle_override ? 1 : 0;
    TEST_ASSERT_FALSE(o.toggle_parking);
  }
  TEST_ASSERT_EQUAL(1, toggles);
  TEST_ASSERT_FALSE(c.tick(3500, o));  // released after the hold: no tap
  TEST_ASSERT_FALSE(o.toggle_parking);
  c.onEvent({ir_codes::kStar, false, 4000}, o);  // a short press 1.9 s long is still a tap
  for (uint32_t t = 4108; t < 5900; t += 108) {
    c.onEvent({ir_codes::kStar, true, t}, o);
    TEST_ASSERT_FALSE(o.toggle_override);
  }
  TEST_ASSERT_TRUE(c.tick(6300, o));
  TEST_ASSERT_TRUE(o.toggle_parking);
}

void test_ir_release_clears_one_shots() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::kUp, false, 0}, o);
  c.onEvent({ir_codes::k7, false, 400}, o);  // mute pressed right after ▲ (no 350 ms gap in this pass)
  TEST_ASSERT_TRUE(o.toggle_mute);
  TEST_ASSERT_TRUE(c.tick(400, o));          // ▲ released in the same pass
  TEST_ASSERT_FALSE(o.toggle_mute);          // not applied twice
  TEST_ASSERT_FALSE(o.active);
}

void test_corrected_codes() {
  TEST_ASSERT_EQUAL_HEX32(0x00FF18E7UL, ir_codes::k5);
  TEST_ASSERT_EQUAL_HEX32(0x00FF38C7UL, ir_codes::k8);
  TEST_ASSERT_EQUAL_HEX32(0x00FF5AA5UL, ir_codes::k9);
  TEST_ASSERT_EQUAL_HEX32(0x00FF4AB5UL, ir_codes::k0);
  TEST_ASSERT_EQUAL_HEX32(0x00FF52ADUL, ir_codes::kHash);
}

namespace {
BootButton::Action holdFor(BootButton& b, uint32_t from_ms, uint32_t ms, uint8_t& thresholds) {
  BootButton::Action last = BootButton::Action::kNone;
  for (uint32_t t = from_ms; t < from_ms + ms; t += 20) {
    const BootButton::Action a = b.update(true, t);
    if (a == BootButton::Action::kWifiThreshold || a == BootButton::Action::kAdminThreshold) ++thresholds;
  }
  for (uint32_t t = from_ms + ms; t < from_ms + ms + 100; t += 20) {
    const BootButton::Action a = b.update(false, t);
    if (a != BootButton::Action::kNone) last = a;
  }
  return last;
}
}  // namespace

void test_short_press_after_boot() {
  BootButton b;
  for (uint32_t t = 0; t < 1000; t += 20) b.update(false, t);
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kPress), static_cast<int>(holdFor(b, 1000, 200, th)));
}

void test_boot_hold_5s_requests_wifi_reset() {
  BootButton b;
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kWifiReset), static_cast<int>(holdFor(b, 0, 6000, th)));
  TEST_ASSERT_EQUAL(1, th);
}

void test_boot_hold_15s_requests_admin_reset() {
  BootButton b;
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kAdminReset), static_cast<int>(holdFor(b, 0, 16000, th)));
  TEST_ASSERT_EQUAL(2, th);
}

void test_long_press_after_boot_is_ignored() {
  BootButton b;
  for (uint32_t t = 0; t < 1000; t += 20) b.update(false, t);
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kNone), static_cast<int>(holdFor(b, 1000, 6000, th)));
  TEST_ASSERT_EQUAL(0, th);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ir_hold_up_drives_then_release_stops);
  RUN_TEST(test_ir_directions_speed_and_ok);
  RUN_TEST(test_ir_speed_limits);
  RUN_TEST(test_ir_camera_jog_while_held);
  RUN_TEST(test_ir_one_shot_keys_ignore_repeats);
  RUN_TEST(test_ir_hash_hold_3s_requests_calibration_once);
  RUN_TEST(test_ir_star_hold_2s_toggles_override_once);
  RUN_TEST(test_ir_release_clears_one_shots);
  RUN_TEST(test_corrected_codes);
  RUN_TEST(test_short_press_after_boot);
  RUN_TEST(test_boot_hold_5s_requests_wifi_reset);
  RUN_TEST(test_boot_hold_15s_requests_admin_reset);
  RUN_TEST(test_long_press_after_boot_is_ignored);
  return UNITY_END();
}
