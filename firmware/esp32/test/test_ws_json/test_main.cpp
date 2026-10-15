// WsJson: frames/snapshot -> the JSON the Basic Drive page reads (bridge LLD R2.9).
#include <string.h>
#include <unity.h>

#include "core/ws_json.h"

using namespace cq::core;
namespace proto = cq::proto;

namespace {
char buf[kWsJsonMax];
const OwnerView kNone{"none", ""};
}  // namespace

void setUp() { memset(buf, 0, sizeof(buf)); }
void tearDown() {}

void test_ack_reason_table_matches_generated_codes() {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(proto::AckStatus::kRoleForbidden); ++i) {
    TEST_ASSERT_EQUAL(proto::kAckStatusCode[i], ackReasonCode(i));
  }
}

void test_tf_scaling_and_sentinels() {
  TankSnapshot s;
  TEST_ASSERT_EQUAL(0, jsonTf(buf, sizeof(buf), 1, s));  // nothing before the first TELEM_FAST
  s.fast_valid = true;
  s.fast.uno_ms = 1234;
  s.fast.yaw = 9050;     // 90.50 deg
  s.fast.pitch = -125;   // -1.25 deg
  s.fast.roll = 300;     // 3.00 deg
  s.fast.dist_mm = 424;  // 42 cm
  s.fast.pan = -155;     // -15.5 deg
  s.fast.tilt = 200;
  s.fast.yaw_rate = -125;  // 0.1 deg/s → -12.5 (spec 01 §6.1, Plan F)
  s.fast.spd_cmd_l = 40;
  s.fast.spd_cmd_r = -40;
  s.fast.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kCamMoving);
  jsonTf(buf, sizeof(buf), 7, s);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"tf\",\"seq\":7,\"tank_ms\":1234,\"yaw\":90.50,\"yaw_rate\":-12.5,\"pitch\":-1.25,\"roll\":3.00,"
      "\"dist_cm\":42,\"pan\":-15.5,\"tilt\":20.0,\"spd_cmd\":[40,-40],\"cam_moving\":true}",
      buf);
  s.fast.dist_mm = 65535;  // clear
  jsonTf(buf, sizeof(buf), 8, s);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dist_cm\":null"));
  TEST_ASSERT_NULL(strstr(buf, "dist_fault"));  // clear is not a fault
  s.fast.dist_mm = 65534;  // sensor fault: the page must not show "CLEAR" (review I5)
  jsonTf(buf, sizeof(buf), 9, s);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dist_cm\":null,\"dist_fault\":true"));
}

void test_ts_flags_owner_and_battery() {
  TankSnapshot s;
  s.slow_valid = true;
  s.slow.batt_mv = 7620;
  s.slow.flags2 = (1u << static_cast<unsigned>(proto::SlowFlagBit::kBattLow)) |
                  (1u << static_cast<unsigned>(proto::SlowFlagBit::kBrakeOff));
  s.slow.env_speed_cap = 100;
  s.fast_valid = true;
  s.fast.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);
  jsonTs(buf, sizeof(buf), s, OwnerView{"human", "phone-1"}, false);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ts\",\"batt_v\":7.62,\"owner\":\"human\",\"owner_client\":\"phone-1\","
      "\"flags\":[\"blocked\",\"batt_low\",\"brake_off\"],"
      "\"envelope\":{\"speed_cap\":100,\"accel_cap\":0,\"lock_brake\":0}}",
      buf);
  jsonTs(buf, sizeof(buf), TankSnapshot{}, kNone, true);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"batt_v\":null"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"flags\":[\"body_offline\"]"));
}

void test_owner_names() {
  TEST_ASSERT_EQUAL_STRING("none", ownerName(proto::Owner::kNone));
  TEST_ASSERT_EQUAL_STRING("human", ownerName(proto::Owner::kNetwork));
  TEST_ASSERT_EQUAL_STRING("ir", ownerName(proto::Owner::kIr));
  TEST_ASSERT_EQUAL_STRING("human", ownerName(proto::Owner::kCalibration));
}

void test_events_in_the_slice() {
  jsonUnoEvent(buf, sizeof(buf), 3, 500, static_cast<uint8_t>(proto::EventKind::kBlocked), 284, 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ev\",\"seq\":3,\"tank_ms\":500,\"kind\":\"blocked\",\"dist_cm\":28}", buf);
  jsonUnoEvent(buf, sizeof(buf), 4, 600, static_cast<uint8_t>(proto::EventKind::kBattLow), 7150,
               0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"batt_low\",\"v\":7.15"));
  jsonUnoEvent(buf, sizeof(buf), 5, 700,
               static_cast<uint8_t>(proto::EventKind::kSafetyOverride), 0, 2);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"on\":false,\"reason\":\"idle\""));
  TEST_ASSERT_EQUAL(0, jsonUnoEvent(buf, sizeof(buf), 6, 0,
                                    static_cast<uint8_t>(proto::EventKind::kSettingsSaved), 0, 0));
  TEST_ASSERT_EQUAL(0, jsonUnoEvent(buf, sizeof(buf), 6, 0,
                                    static_cast<uint8_t>(proto::EventKind::kControlChanged), 1, 0));
}

void test_control_changed_and_body() {
  jsonControlChanged(buf, sizeof(buf), 9, 10, OwnerView{"human", "bench"});
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ev\",\"seq\":9,\"tank_ms\":10,\"kind\":\"control_changed\",\"owner\":\"human\","
      "\"client\":\"bench\"}",
      buf);
  jsonBody(buf, sizeof(buf), 1, 0, false, 1);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"BODY_INCOMPATIBLE\",\"uno_proto\":1"));
  jsonBody(buf, sizeof(buf), 2, 0, true, -1);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"body_online\""));
}

void test_ack_and_nacks() {
  jsonAck(buf, sizeof(buf), 7);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ack\",\"id\":7}", buf);
  jsonNack(buf, sizeof(buf), 7, BridgeReason::kNotOwner);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"nack\",\"id\":7,\"code\":\"COMMAND_3001\",\"reason\":\"NOT_OWNER\","
      "\"message\":\"Another source has control\"}",
      buf);
  jsonNack(buf, sizeof(buf), -1, BridgeReason::kBusy);  // streaming: no id
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"nack\",\"code\":\"COMMAND_3005\",\"reason\":\"BUSY\",\"message\":\"Busy, try "
      "again\"}",
      buf);
  jsonNackStatus(buf, sizeof(buf), 4, static_cast<uint8_t>(proto::AckStatus::kBlocked));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"COMMAND_3002\",\"reason\":\"BLOCKED\""));
  jsonNackStatus(buf, sizeof(buf), 5, kStatusTimeout);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"COMMAND_TIMEOUT\""));
  jsonNack(buf, sizeof(buf), 6, BridgeReason::kBodyOffline);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"COMMAND_3007\""));
}

void test_welcome_and_pong() {
  HelloInfo h;
  h.seen = true;
  h.fw = 0x0100;
  h.caps = 0;
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"bridge-v0.2.0", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"welcome\",\"role\":\"human\",\"you\":\"phone-1\",\"tank\":{\"name\":"
      "\"conqueror-bridge\",\"fw\":{\"esp32\":\"bridge-v0.2.0\",\"uno\":\"1.0\"},\"proto\":2,"
      "\"caps\":{\"encoders\":false,\"turn\":false,\"move\":false,\"drive_rate\":false,\"calibrate\":false,"
      "\"audio_led\":false,\"sensor\":\"OV3660\"}},\"owner\":\"none\",\"owner_client\":"
      "\"\"}",
      buf);
  HelloInfo none;
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &none, "none", kNone, "phone-2"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"uno\":null"));
  jsonPong(buf, sizeof(buf), "1728734400123.5", 99);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"pong\",\"t0\":1728734400123.5,\"tank_ms\":99}", buf);
}

void test_too_small_buffer_returns_zero() {
  char small[16];
  TEST_ASSERT_EQUAL(0, jsonNack(small, sizeof(small), 1, BridgeReason::kBusy));
}

// Review M10: strings from outside (SSID) are escaped, so /info stays valid JSON.
void test_json_escape() {
  char out[32];
  TEST_ASSERT_EQUAL(11, cq::core::jsonEscape(out, sizeof(out), "my \"wifi\\"));
  TEST_ASSERT_EQUAL_STRING("my \\\"wifi\\\\", out);
  cq::core::jsonEscape(out, sizeof(out), "a\x01" "b");
  TEST_ASSERT_EQUAL_STRING("a\\u0001b", out);
  cq::core::jsonEscape(out, 5, "abc\"def");  // never splits an escape at the end
  TEST_ASSERT_EQUAL_STRING("abc", out);
}

// Plan F: the page hides features from the tank's HELLO CapBits (Uno Lite = AUDIO_LED only).
// R3.1: the tank's audio state reaches the page (speaker chip).
void test_ts_muted_quiet_flags() {
  TankSnapshot s;
  s.slow_valid = true;
  s.slow.flags2 = 1u << static_cast<unsigned>(proto::SlowFlagBit::kMuted);
  jsonTs(buf, sizeof(buf), s, kNone, false);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"flags\":[\"muted\"]"));
  s.slow.flags2 = 1u << static_cast<unsigned>(proto::SlowFlagBit::kQuiet);
  jsonTs(buf, sizeof(buf), s, kNone, false);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"flags\":[\"quiet\"]"));
}

void test_welcome_caps_bits() {
  HelloInfo h;
  h.seen = true;
  h.fw = 0x0100;
  h.caps = 1u << static_cast<uint8_t>(proto::CapBit::kAudioLed);
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"audio_led\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"turn\":false"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"encoders\":false"));
  h.caps = 0xFF;  // full firmware with encoders
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"turn\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"encoders\":true"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ack_reason_table_matches_generated_codes);
  RUN_TEST(test_tf_scaling_and_sentinels);
  RUN_TEST(test_ts_flags_owner_and_battery);
  RUN_TEST(test_owner_names);
  RUN_TEST(test_events_in_the_slice);
  RUN_TEST(test_control_changed_and_body);
  RUN_TEST(test_ack_and_nacks);
  RUN_TEST(test_welcome_and_pong);
  RUN_TEST(test_welcome_caps_bits);
  RUN_TEST(test_ts_muted_quiet_flags);
  RUN_TEST(test_too_small_buffer_returns_zero);
  RUN_TEST(test_json_escape);
  return UNITY_END();
}
