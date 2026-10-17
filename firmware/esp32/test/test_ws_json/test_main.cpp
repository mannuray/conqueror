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

// Plan I (spec 01 §6.3, spec 02 §F1): the Uno's stall / encoder fault / heading runaway reach the phone.
void test_encoder_safety_events() {
  jsonUnoEvent(buf, sizeof(buf), 7, 800, static_cast<uint8_t>(proto::EventKind::kStall), 1, 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ev\",\"seq\":7,\"tank_ms\":800,\"kind\":\"stall\",\"side\":\"right\"}", buf);
  jsonUnoEvent(buf, sizeof(buf), 8, 900, static_cast<uint8_t>(proto::EventKind::kEncoderFault), 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"encoder_fault\",\"side\":\"left\"}"));
  jsonUnoEvent(buf, sizeof(buf), 9, 950, static_cast<uint8_t>(proto::EventKind::kHeadingFault), -34, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"heading_fault\",\"err_deg\":-34}"));
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
      "\"conqueror-bridge\",\"fw\":{\"esp32\":\"bridge-v0.2.0\",\"uno\":\"1.0\"},\"proto\":3,"
      "\"caps\":{\"encoders\":false,\"turn\":true,\"move\":false,\"drive_rate\":true,\"calibrate\":false,"
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

// Bridge LLD R6.5: an encoder the Uno switched off (sticky until restart) reaches the page.
void test_ts_encoder_fault_flag() {
  TankSnapshot s;
  s.fast_valid = true;
  s.fast.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kEncoderFault);
  jsonTs(buf, sizeof(buf), s, kNone, false);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"flags\":[\"encoder_fault\"]"));
}

// Plan K: caps.move is true when the bridge runs MOVE (Uno with encoders, no MOVE of its own).
void test_welcome_caps_move_with_encoders() {
  HelloInfo h;
  h.seen = true;
  h.fw = 0x0100;
  h.caps = (1u << static_cast<uint8_t>(proto::CapBit::kAudioLed)) | (1u << static_cast<uint8_t>(proto::CapBit::kEncoders));
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":true"));
  h.caps = 1u << static_cast<uint8_t>(proto::CapBit::kAudioLed);
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":false"));
}

void test_welcome_caps_bits() {
  HelloInfo h;
  h.seen = true;
  h.fw = 0x0100;
  h.caps = 1u << static_cast<uint8_t>(proto::CapBit::kAudioLed);
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"audio_led\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"turn\":true"));        // Plan H: the bridge's motion service runs it
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"drive_rate\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":false"));       // needs encoders
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"encoders\":false"));
  h.seen = false;  // no body yet: nothing to turn
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"turn\":false"));
  h.seen = true;
  h.caps = 0xFF;  // full firmware with encoders: its TURN / DRIVE_RATE are not forwarded yet (Plan H review M7)
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"turn\":false"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"drive_rate\":false"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"encoders\":true"));
  jsonNack(buf, sizeof(buf), 4, BridgeReason::kUnavailable);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"COMMAND_3003\",\"reason\":\"UNAVAILABLE\""));
}

// Plan H (bridge LLD R4.5 / R4.6): primitive_done from the bridge's motion service, tf.primitive progress.
void test_primitive_done_and_tf_progress() {
  PrimitiveEnd e;
  e.kind = PrimKind::kTurn;
  e.ok = true;
  e.reason = EndReason::kDone;
  e.id = 5;
  jsonPrimitiveDone(buf, sizeof(buf), 9, 1234, e);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ev\",\"seq\":9,\"tank_ms\":1234,\"kind\":\"primitive_done\",\"prim\":\"turn\",\"ok\":true,"
      "\"reason\":\"done\",\"id\":5}",
      buf);
  e.kind = PrimKind::kRate;
  e.ok = false;
  e.reason = EndReason::kNotOwner;
  e.id = -1;
  jsonPrimitiveDone(buf, sizeof(buf), 10, 1300, e);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ev\",\"seq\":10,\"tank_ms\":1300,\"kind\":\"primitive_done\",\"prim\":\"rate\",\"ok\":false,"
      "\"reason\":\"not_owner\"}",
      buf);
  TankSnapshot s;
  s.fast_valid = true;
  jsonTf(buf, sizeof(buf), 1, s);
  TEST_ASSERT_NULL(strstr(buf, "primitive"));
  jsonTf(buf, sizeof(buf), 2, s, PrimitiveView{true, PrimKind::kTurn, 40});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cam_moving\":false,\"primitive\":{\"kind\":\"turn\",\"pct\":40}}"));
}

// Bridge LLD R5: debug messages carry the raw TELEM_FAST / TELEM_SLOW values.
void test_td_raw_fast_values() {
  TankSnapshot s;
  TEST_ASSERT_EQUAL(0, jsonTd(buf, sizeof(buf), 1, s));  // nothing before the first TELEM_FAST
  s.fast_valid = true;
  s.fast.uno_ms = 1234;
  s.fast.enc_l = 100;
  s.fast.enc_r = -5;
  s.fast.pwm_l = 130;
  s.fast.pwm_r = -130;
  s.fast.spd_cmd_l = 25;
  s.fast.spd_cmd_r = -25;
  s.fast.spd_act_l = -128;  // unknown
  s.fast.spd_act_r = 12;
  s.fast.yaw = 9050;
  s.fast.yaw_rate = -125;
  s.fast.pitch = -125;
  s.fast.roll = 300;
  s.fast.dist_mm = 424;
  s.fast.closing_cm_s = -7;
  s.fast.pan = -155;
  s.fast.tilt = 200;
  s.fast.flags = 5;
  s.fast.prim = 1;
  s.fast.prim_pct = 40;
  jsonTd(buf, sizeof(buf), 3, s);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"td\",\"seq\":3,\"uno_ms\":1234,\"enc\":[100,-5],\"pwm\":[130,-130],\"cmd\":[25,-25],"
      "\"act\":[null,12],\"yaw\":90.50,\"rate\":-12.5,\"pitch\":-1.25,\"roll\":3.00,\"dist_mm\":424,"
      "\"closing\":-7,\"pan\":-15.5,\"tilt\":20.0,\"flags\":5,\"prim\":1,\"pct\":40}",
      buf);
  s.fast.dist_mm = 65534;  // sensor fault
  jsonTd(buf, sizeof(buf), 4, s);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dist_mm\":null,\"dist_fault\":true"));
  s.fast.dist_mm = 65535;  // no echo
  jsonTd(buf, sizeof(buf), 5, s);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dist_mm\":null,\"closing"));
}

void test_tds_raw_slow_values() {
  TankSnapshot s;
  TEST_ASSERT_EQUAL(0, jsonTds(buf, sizeof(buf), s));
  s.slow_valid = true;
  s.slow.uptime_s = 77;
  s.slow.reset_reason = 1;
  s.slow.batt_mv = 7860;
  s.slow.driving = 1;
  s.slow.temp = -3;
  s.slow.owner = 2;
  s.slow.flags2 = 512;
  s.slow.accel_var = 9;
  s.slow.loop_load = 41;
  s.slow.loop_max_us = 1800;
  s.slow.free_ram = 259;
  s.slow.err_frames = 2;
  s.slow.err_i2c = 0;
  s.slow.settings_crc = 4660;
  s.slow.env_speed_cap = 60;
  s.slow.env_accel_cap = 50;
  s.slow.env_lock_brake = 1;
  s.slow.profile_idx = 0;
  s.slow.cal_status[0] = 1;
  s.slow.cal_status[1] = 0;
  s.slow.cal_status[2] = 2;
  s.slow.hw = 3;
  jsonTds(buf, sizeof(buf), s);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"tds\",\"uptime_s\":77,\"reset_reason\":1,\"batt_mv\":7860,\"driving\":1,\"temp\":-3,"
      "\"owner\":2,\"flags2\":512,\"accel_var\":9,\"loop_load\":41,\"loop_max_us\":1800,\"free_ram\":259,"
      "\"err_frames\":2,\"err_i2c\":0,\"settings_crc\":4660,\"env\":[60,50,1],\"profile\":0,\"cal\":[1,0,2],"
      "\"hw\":3}",
      buf);
}

// Worst-case values still fit kWsJsonMax (a truncated message would be dropped: fmt returns 0).
void test_td_tds_fit_worst_case() {
  TankSnapshot s;
  s.fast_valid = true;
  s.slow_valid = true;
  s.fast.uno_ms = 4294967295u;
  s.fast.enc_l = -2147483647 - 1;
  s.fast.enc_r = -2147483647 - 1;
  s.fast.pwm_l = s.fast.pwm_r = -32768;
  s.fast.spd_cmd_l = s.fast.spd_cmd_r = s.fast.spd_act_l = s.fast.spd_act_r = -127;
  s.fast.yaw = 65535;
  s.fast.yaw_rate = s.fast.pitch = s.fast.roll = s.fast.closing_cm_s = s.fast.pan = s.fast.tilt = -32768;
  s.fast.dist_mm = 65534;
  s.fast.flags = 65535;
  s.fast.prim = s.fast.prim_pct = 255;
  TEST_ASSERT_TRUE(jsonTd(buf, sizeof(buf), 4294967295u, s) > 0);
  s.slow.uptime_s = s.slow.flags2 = 4294967295u;
  s.slow.temp = -32768;
  s.slow.batt_mv = s.slow.accel_var = s.slow.loop_max_us = s.slow.free_ram = 65535;
  s.slow.err_frames = s.slow.err_i2c = s.slow.settings_crc = 65535;
  TEST_ASSERT_TRUE(jsonTds(buf, sizeof(buf), s) > 0);
}

// Plan K (bridge LLD R7.1): move ends, progress, calib reply.
void test_primitive_done_move_with_moved_cm() {
  PrimitiveEnd e;
  e.kind = PrimKind::kMove;
  e.ok = true;
  e.reason = EndReason::kDone;
  e.id = 7;
  e.has_moved = true;
  e.moved_mm = 996;
  jsonPrimitiveDone(buf, sizeof(buf), 3, 500, e);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ev\",\"seq\":3,\"tank_ms\":500,\"kind\":\"primitive_done\",\"prim\":\"move\",\"ok\":true,"
      "\"reason\":\"done\",\"moved_cm\":99.6,\"id\":7}",
      buf);
  e.ok = false;
  e.id = -1;
  e.moved_mm = -503;
  e.reason = EndReason::kBlocked;
  jsonPrimitiveDone(buf, sizeof(buf), 4, 600, e);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"blocked\",\"moved_cm\":-50.3}"));
  e.reason = EndReason::kEncoderFault;
  jsonPrimitiveDone(buf, sizeof(buf), 5, 700, e);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"encoder_fault\""));
}

void test_tf_move_progress_and_calib_reply() {
  TankSnapshot s;
  s.fast_valid = true;
  jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kMove, 42});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"move\",\"pct\":42}"));
  jsonCalib(buf, sizeof(buf), 7480, 25);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"calib\",\"counts_per_m\":7480,\"roll_mm\":25}", buf);
}

void test_tf_primitive_limited() {
  TankSnapshot s;
  s.fast_valid = true;
  jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kRate, 0, true});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"rate\",\"pct\":0,\"limited\":true}"));
  jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kTurn, 42, false});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"turn\",\"pct\":42}"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ack_reason_table_matches_generated_codes);
  RUN_TEST(test_tf_scaling_and_sentinels);
  RUN_TEST(test_ts_flags_owner_and_battery);
  RUN_TEST(test_owner_names);
  RUN_TEST(test_events_in_the_slice);
  RUN_TEST(test_encoder_safety_events);
  RUN_TEST(test_control_changed_and_body);
  RUN_TEST(test_ack_and_nacks);
  RUN_TEST(test_welcome_and_pong);
  RUN_TEST(test_welcome_caps_bits);
  RUN_TEST(test_welcome_caps_move_with_encoders);
  RUN_TEST(test_ts_muted_quiet_flags);
  RUN_TEST(test_td_raw_fast_values);
  RUN_TEST(test_tds_raw_slow_values);
  RUN_TEST(test_td_tds_fit_worst_case);
  RUN_TEST(test_primitive_done_and_tf_progress);
  RUN_TEST(test_too_small_buffer_returns_zero);
  RUN_TEST(test_json_escape);
  RUN_TEST(test_ts_encoder_fault_flag);
  RUN_TEST(test_primitive_done_move_with_moved_cm);
  RUN_TEST(test_tf_move_progress_and_calib_reply);
  RUN_TEST(test_tf_primitive_limited);
  return UNITY_END();
}
