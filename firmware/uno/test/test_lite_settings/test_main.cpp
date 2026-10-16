#include <stddef.h>
#include <string.h>
#include <unity.h>

#include "domain/lite/settings_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
uint8_t g_mem[64];
uint8_t rd(uint16_t a) { return g_mem[a]; }
uint8_t id(proto::SettingId s) { return static_cast<uint8_t>(s); }
}  // namespace

void test_settings_defaults() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL_UINT8(110, s.values().db_start[0]);
  TEST_ASSERT_EQUAL_UINT8(90, s.values().db_keep[3]);
  TEST_ASSERT_EQUAL_UINT16(3500, s.values().heading_kp_milli);
  TEST_ASSERT_EQUAL_UINT16(1200, s.values().rate_kp_milli);
  TEST_ASSERT_EQUAL_UINT16(200, s.values().accel_limit);
  TEST_ASSERT_EQUAL_UINT8(40, s.values().turn_full);
  TEST_ASSERT_EQUAL_UINT8(15, s.values().obstacle_stop);
  TEST_ASSERT_EQUAL_UINT8(50, s.values().obstacle_slow);
}

void test_settings_set_get_and_range() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kGyroBiasZ), -17));
  int32_t v = 0;
  TEST_ASSERT_TRUE(s.get(id(proto::SettingId::kGyroBiasZ), v));
  TEST_ASSERT_EQUAL_INT32(-17, v);
  TEST_ASSERT_EQUAL(proto::AckStatus::kSettingOutOfRange, s.set(id(proto::SettingId::kObstacleStop), 4));
  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kDeadbandStartRRev), 120));
  TEST_ASSERT_EQUAL_UINT8(120, s.values().db_start[3]);
}

void test_settings_unknown_id_is_unavailable() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL(proto::AckStatus::kUnavailable, s.set(id(proto::SettingId::kServoSpeed), 100));
  int32_t v = 0;
  TEST_ASSERT_FALSE(s.get(id(proto::SettingId::kServoSpeed), v));
}

void test_settings_round_trip_through_eeprom_block() {
  SettingsLite a;
  a.defaults();
  a.set(id(proto::SettingId::kPidHeadingKp), 4000);
  memset(g_mem, 0xFF, sizeof g_mem);
  const uint8_t n = a.encode(g_mem + SettingsLite::kAddr);
  TEST_ASSERT_EQUAL_UINT8(SettingsLite::kBlockSize, n);
  SettingsLite b;
  TEST_ASSERT_TRUE(b.load(rd));
  TEST_ASSERT_EQUAL_UINT16(4000, b.values().heading_kp_milli);
}

void test_settings_garbage_eeprom_gives_defaults() {
  SettingsLite s;
  memset(g_mem, 0xA5, sizeof g_mem);  // someone else's bytes (ELEGOO / full firmware layout)
  TEST_ASSERT_FALSE(s.load(rd));
  TEST_ASSERT_EQUAL_UINT16(3500, s.values().heading_kp_milli);
  SettingsLite good;
  good.defaults();
  good.encode(g_mem);
  g_mem[6] ^= 0x01;  // one flipped bit in the data → CRC fails → defaults
  TEST_ASSERT_FALSE(s.load(rd));
}

// Plan E deferred minor: a CRC-valid block with an out-of-range value (e.g. a stop distance of 0 that
// would switch off the obstacle brake) is not trusted: defaults are used, as for a bad CRC.
void test_settings_out_of_range_block_gives_defaults() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL_UINT8(SettingsLite::kBlockSize, s.encode(g_mem));
  g_mem[3 + offsetof(LiteSettings, obstacle_stop)] = 0;  // below the 5 cm minimum
  g_mem[3 + sizeof(LiteSettings)] = proto::crc8(g_mem + 2, static_cast<uint8_t>(sizeof(LiteSettings) + 1));
  SettingsLite r;
  TEST_ASSERT_FALSE(r.load(rd));
  TEST_ASSERT_EQUAL_UINT8(15, r.values().obstacle_stop);
  g_mem[3 + offsetof(LiteSettings, obstacle_stop)] = 20;  // in range: loaded
  g_mem[3 + sizeof(LiteSettings)] = proto::crc8(g_mem + 2, static_cast<uint8_t>(sizeof(LiteSettings) + 1));
  TEST_ASSERT_TRUE(r.load(rd));
  TEST_ASSERT_EQUAL_UINT8(20, r.values().obstacle_stop);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_settings_defaults);
  RUN_TEST(test_settings_set_get_and_range);
  RUN_TEST(test_settings_unknown_id_is_unavailable);
  RUN_TEST(test_settings_round_trip_through_eeprom_block);
  RUN_TEST(test_settings_garbage_eeprom_gives_defaults);
  RUN_TEST(test_settings_out_of_range_block_gives_defaults);
  return UNITY_END();
}
