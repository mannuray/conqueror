// The camera's sensor values and the saved settings' carry-over (bridge LLD R24, design-notes §140).
#include <string.h>
#include <unity.h>

#include "core/cam_tuning.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

void test_normal_light_gains_like_the_sensor_default_without_night_mode() {
  CamSettings s;
  const CamTuning t = sensorTuning(s);
  TEST_ASSERT_EQUAL_UINT16(248, t.gain_ceiling_raw);  // OV3660 raw gain (16 = 1x): its own default 0x00F8 = 15.5x
  TEST_ASSERT_EQUAL_INT8(0, t.ae_level);
  TEST_ASSERT_FALSE(t.night);
  TEST_ASSERT_EQUAL_UINT8(0, t.wb_mode);
}

void test_low_light_gains_up_to_32x_one_step_brighter_and_keeps_the_frame_rate() {
  CamSettings s;
  s.low_light = true;
  const CamTuning t = sensorTuning(s);
  TEST_ASSERT_EQUAL_UINT16(512, t.gain_ceiling_raw);
  TEST_ASSERT_EQUAL_INT8(1, t.ae_level);
  TEST_ASSERT_FALSE(t.night);  // night mode lowers the frame rate: its own switch
}

void test_night_is_its_own_switch() {
  CamSettings s;
  s.night = true;
  TEST_ASSERT_TRUE(sensorTuning(s).night);
  TEST_ASSERT_EQUAL_UINT16(248, sensorTuning(s).gain_ceiling_raw);
}

void test_white_balance_names_and_modes() {
  const char* const names[] = {"auto", "sunny", "cloudy", "office", "home"};
  for (uint8_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_STRING(names[i], wbName(i));
    uint8_t m = 99;
    TEST_ASSERT_TRUE(wbOf(names[i], m));
    TEST_ASSERT_EQUAL_UINT8(i, m);
    CamSettings s;
    s.wb = i;
    TEST_ASSERT_EQUAL_UINT8(i, sensorTuning(s).wb_mode);
  }
  uint8_t m = 7;
  TEST_ASSERT_FALSE(wbOf("Auto", m));  // lower case only, as the names are sent
  TEST_ASSERT_FALSE(wbOf("night", m));
  TEST_ASSERT_EQUAL_STRING("auto", wbName(9));
}

void test_the_old_8_byte_settings_are_carried_over() {
  const uint8_t old[8] = {0, 25, 10, static_cast<uint8_t>(-1), 2, 1, 0, 1};  // QVGA q25 fps10 b-1 c2 low no-vflip mirror
  CamSettings s;
  s.wb = 3;
  s.night = true;
  TEST_ASSERT_TRUE(camFromBlock(old, sizeof(old), s));
  TEST_ASSERT_EQUAL_UINT8(0, s.size);
  TEST_ASSERT_EQUAL_UINT8(25, s.quality);
  TEST_ASSERT_EQUAL_UINT8(10, s.fps_cap);
  TEST_ASSERT_EQUAL_INT8(-1, s.brightness);
  TEST_ASSERT_EQUAL_INT8(2, s.contrast);
  TEST_ASSERT_TRUE(s.low_light);
  TEST_ASSERT_FALSE(s.vflip);
  TEST_ASSERT_TRUE(s.hmirror);
  TEST_ASSERT_EQUAL_UINT8(0, s.wb);  // the new ones take their defaults
  TEST_ASSERT_FALSE(s.night);
}

void test_a_new_block_round_trips_and_other_sizes_are_refused() {
  CamSettings a;
  a.size = 2;
  a.wb = 4;
  a.night = true;
  uint8_t buf[sizeof(CamSettings)];
  memcpy(buf, &a, sizeof(a));
  CamSettings b;
  TEST_ASSERT_TRUE(camFromBlock(buf, sizeof(buf), b));
  TEST_ASSERT_EQUAL_UINT8(2, b.size);
  TEST_ASSERT_EQUAL_UINT8(4, b.wb);
  TEST_ASSERT_TRUE(b.night);
  CamSettings c;
  TEST_ASSERT_FALSE(camFromBlock(buf, 7, c));
  uint8_t bad[8] = {9, 12, 0, 0, 0, 0, 1, 0};  // size 9: not a size
  TEST_ASSERT_FALSE(camFromBlock(bad, sizeof(bad), c));
  buf[sizeof(CamSettings) - 2] = 7;  // wb 7: not a mode
  TEST_ASSERT_FALSE(camFromBlock(buf, sizeof(buf), c));
}

void test_an_on_off_byte_that_is_not_0_or_1_is_refused() {  // Plan X review M1: no invalid bool from flash
  uint8_t old[8] = {1, 12, 0, 0, 0, 2, 1, 0};  // low_light byte 2
  CamSettings c;
  TEST_ASSERT_FALSE(camFromBlock(old, sizeof(old), c));
  CamSettings a;
  uint8_t buf[sizeof(CamSettings)];
  memcpy(buf, &a, sizeof(a));
  buf[sizeof(CamSettings) - 1] = 5;  // night byte 5
  TEST_ASSERT_FALSE(camFromBlock(buf, sizeof(buf), c));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_normal_light_gains_like_the_sensor_default_without_night_mode);
  RUN_TEST(test_low_light_gains_up_to_32x_one_step_brighter_and_keeps_the_frame_rate);
  RUN_TEST(test_night_is_its_own_switch);
  RUN_TEST(test_white_balance_names_and_modes);
  RUN_TEST(test_the_old_8_byte_settings_are_carried_over);
  RUN_TEST(test_a_new_block_round_trips_and_other_sizes_are_refused);
  RUN_TEST(test_an_on_off_byte_that_is_not_0_or_1_is_refused);
  return UNITY_END();
}
