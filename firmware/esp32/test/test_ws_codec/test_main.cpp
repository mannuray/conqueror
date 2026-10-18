// WsCodec: the Basic Drive JSON slice (bridge LLD R2.9) in, robust against junk.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "core/ws_codec.h"

using cq::core::parseWsCommand;
using cq::core::WsCmdKind;
using cq::core::WsCommand;

namespace {
WsCommand P(const char* s) { return parseWsCommand(s, strlen(s)); }
int K(WsCmdKind k) { return static_cast<int>(k); }
}  // namespace

void setUp() {}
void tearDown() {}

void test_drive() {
  const WsCommand c = P("{\"t\":\"drive\",\"v\":40,\"w\":-12}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDrive), K(c.kind));
  TEST_ASSERT_EQUAL(40, c.v);
  TEST_ASSERT_EQUAL(-12, c.w);
  TEST_ASSERT_FALSE(c.has_id);
}

void test_drive_clamps_and_rounds() {
  const WsCommand c = P("{ \"t\" : \"drive\" , \"v\" : 250.4 , \"w\" : -33.6 }");
  TEST_ASSERT_EQUAL(100, c.v);
  TEST_ASSERT_EQUAL(-34, c.w);
}

void test_drive_needs_v_and_w() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"drive\",\"v\":10}").kind));
}

void test_stop_with_id() {
  const WsCommand c = P("{\"t\":\"stop\",\"id\":7}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kStop), K(c.kind));
  TEST_ASSERT_TRUE(c.has_id);
  TEST_ASSERT_EQUAL(7, c.id);
}

void test_cam_joy() {
  const WsCommand c = P("{\"t\":\"cam_joy\",\"x\":-100,\"y\":55,\"active\":true}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamJoy), K(c.kind));
  TEST_ASSERT_EQUAL(-100, c.x);
  TEST_ASSERT_EQUAL(55, c.y);
  TEST_ASSERT_TRUE(c.active);
  TEST_ASSERT_FALSE(P("{\"t\":\"cam_joy\",\"x\":0,\"y\":0,\"active\":false}").active);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_joy\",\"x\":0,\"y\":0}").kind));
}

void test_simple_kinds() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamHome), K(P("{\"t\":\"cam_home\",\"id\":3}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kRelease), K(P("{\"t\":\"release_control\"}").kind));
}

void test_hello_ignores_key_and_nested() {
  const WsCommand c = P(
      "{\"t\":\"hello\",\"key\":\"abc\\\"def\",\"client\":\"basic-drive\",\"proto\":1,"
      "\"extra\":{\"a\":[1,{\"b\":\"}\"}]}}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kHello), K(c.kind));
  TEST_ASSERT_EQUAL_STRING("basic-drive", c.client);
}

void test_ping_echoes_t0_verbatim() {
  const WsCommand c = P("{\"t\":\"ping\",\"t0\":1728734400123.5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kPing), K(c.kind));
  TEST_ASSERT_EQUAL_STRING("1728734400123.5", c.t0);
  TEST_ASSERT_EQUAL_STRING("null", P("{\"t\":\"ping\"}").t0);
}

// Review M4: id must be a non-negative integer that fits int32 (no UB cast, no silent "no id").
void test_id_out_of_range_is_invalid() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"stop\",\"id\":1e30}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"stop\",\"id\":-1}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"stop\",\"id\":2.5}").kind));
  const WsCommand c = P("{\"t\":\"stop\",\"id\":2147483647}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kStop), K(c.kind));
  TEST_ASSERT_EQUAL_INT32(2147483647, c.id);
}

// Review M5: t0 is re-formatted, so pong always carries a valid JSON number.
void test_ping_t0_is_valid_json_number() {
  TEST_ASSERT_EQUAL_STRING("1", P("{\"t\":\"ping\",\"t0\":+1}").t0);
  TEST_ASSERT_EQUAL_STRING("0.5", P("{\"t\":\"ping\",\"t0\":.5}").t0);
  TEST_ASSERT_EQUAL_STRING("5", P("{\"t\":\"ping\",\"t0\":5.}").t0);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"ping\",\"t0\":1e999}").kind));
}

// Review M7: a stop longer than LinkCommand::text (200 B), e.g. with a future "key" field, is still
// recognised from the raw frame, so the bridge forwards it as a short stop instead of nacking it.
void test_long_stop_is_still_a_stop() {
  char big[320];
  int n = snprintf(big, sizeof(big), "{\"t\":\"stop\",\"id\":9,\"key\":\"");
  while (n < 300) big[n++] = 'k';
  big[n++] = '"';
  big[n++] = '}';
  const WsCommand c = parseWsCommand(big, static_cast<size_t>(n));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kStop), K(c.kind));
  TEST_ASSERT_TRUE(c.has_id);
  TEST_ASSERT_EQUAL_INT32(9, c.id);
}

void test_unsupported_commands() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kUnsupported), K(P("{\"t\":\"go_to\",\"x\":1}").kind));  // move is parsed since Plan K
  TEST_ASSERT_EQUAL(K(WsCmdKind::kUnsupported), K(P("{\"t\":\"safety_override\"}").kind));
}

void test_junk_is_invalid() {
  const char* junk[] = {"", "x", "{", "{}", "{\"t\":}", "{\"t\":\"drive\",\"v\":1,\"w\":2",
                        "{\"t\":\"drive\",\"v\":\"a\",\"w\":2}", "{\"t\":\"stop\"} trailing",
                        "[\"t\",\"stop\"]", "{\"t\":\"drive\",\"v\":1e,\"w\":2}"};
  for (const char* j : junk) TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P(j).kind));
}

void test_length_is_respected() {
  const char* s = "{\"t\":\"stop\"}GARBAGE";
  TEST_ASSERT_EQUAL(K(WsCmdKind::kStop), K(parseWsCommand(s, 12).kind));
}

// R3.1 (spec 01 §4): audio_mode normal/quiet/mute -> proto::AudioModeValue 0/1/2.
void test_audio_mode_parse() {
  const WsCommand m = P("{\"t\":\"audio_mode\",\"mode\":\"mute\",\"id\":3}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAudioMode), K(m.kind));
  TEST_ASSERT_EQUAL_UINT8(2, m.audio_mode);
  TEST_ASSERT_TRUE(m.has_id);
  TEST_ASSERT_EQUAL_UINT8(0, P("{\"t\":\"audio_mode\",\"mode\":\"normal\"}").audio_mode);
  TEST_ASSERT_EQUAL_UINT8(1, P("{\"t\":\"audio_mode\",\"mode\":\"quiet\"}").audio_mode);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAudioMode), K(P("{\"t\":\"audio_mode\",\"mode\":\"quiet\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"audio_mode\",\"mode\":\"loud\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"audio_mode\"}").kind));
}

// Plan F (spec 01 §4): sound by catalogue name, cam_lock on/off.
void test_sound_by_name() {
  const WsCommand c = P("{\"t\":\"sound\",\"name\":\"fanfare\",\"id\":7}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSound), K(c.kind));
  TEST_ASSERT_EQUAL_UINT8(13, c.sound_id);  // proto::SoundId::kFanfare
  TEST_ASSERT_TRUE(c.has_id);
  const WsCommand g = P("{\"t\":\"sound\",\"name\":\"goodbye\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSound), K(g.kind));
  TEST_ASSERT_EQUAL_UINT8(23, g.sound_id);
  TEST_ASSERT_EQUAL_UINT8(1, P("{\"t\":\"sound\",\"name\":\"startup\"}").sound_id);
}

void test_sound_unknown_name_invalid() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"sound\",\"name\":\"bogus\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"sound\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"sound\",\"name\":7}").kind));
}

void test_cam_lock() {
  const WsCommand c = P("{\"t\":\"cam_lock\",\"on\":true,\"id\":2}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamLock), K(c.kind));
  TEST_ASSERT_TRUE(c.on);
  TEST_ASSERT_FALSE(P("{\"t\":\"cam_lock\",\"on\":false}").on);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_lock\"}").kind));  // review M20: "on" is required
}

// Plan H (bridge LLD R4.2): turn / drive_rate / cancel.
void test_turn_by_and_to() {
  WsCommand c = P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(c.kind));
  TEST_ASSERT_FALSE(c.turn_has_to);
  TEST_ASSERT_EQUAL_INT32(9000, c.turn_by_cdeg);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);
  TEST_ASSERT_TRUE(c.has_id);
  c = P("{\"t\":\"turn\",\"to\":270}");
  TEST_ASSERT_TRUE(c.turn_has_to);
  TEST_ASSERT_EQUAL_UINT16(27000, c.turn_to_cdeg);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);  // default
  TEST_ASSERT_EQUAL_INT32(-72000, P("{\"t\":\"turn\",\"by\":-720}").turn_by_cdeg);
  TEST_ASSERT_EQUAL_INT32(4550, P("{\"t\":\"turn\",\"by\":45.5}").turn_by_cdeg);
  TEST_ASSERT_EQUAL_UINT16(30, P("{\"t\":\"turn\",\"by\":90,\"radius_cm\":30}").radius_cm);
}

void test_turn_invalid() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\"}").kind));                    // neither
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"to\":10}").kind));  // both
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":721}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":0}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"to\":360}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"to\":-1}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":5}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":11}").kind));  // < w_min
  TEST_ASSERT_EQUAL_UINT8(12, P("{\"t\":\"turn\",\"by\":90,\"speed\":12}").speed);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":0.5}").kind));  // |by| >= 1°
  TEST_ASSERT_EQUAL_INT32(-100, P("{\"t\":\"turn\",\"by\":-1}").turn_by_cdeg);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":101}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"radius_cm\":-1}").kind));
}

void test_drive_rate_and_cancel() {
  const WsCommand r = P("{\"t\":\"drive_rate\",\"v\":30,\"yaw_rate\":-45.5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDriveRate), K(r.kind));
  TEST_ASSERT_EQUAL_INT8(30, r.v);
  TEST_ASSERT_EQUAL_INT16(-455, r.rate_ddps);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"drive_rate\",\"v\":30,\"yaw_rate\":200}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"drive_rate\",\"v\":30}").kind));  // rate required
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"drive_rate\",\"yaw_rate\":10}").kind));  // v required
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCancel), K(P("{\"t\":\"cancel\",\"id\":3}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCancel), K(P("{\"t\":\"cancel\"}").kind));
}

// Refactor guard (split parseWsCommand): odd keys are skipped, not read as v/w/x/y; bad booleans reject.
void test_odd_keys_and_bools() {
  const WsCommand c = P("{\"\":5,\"vv\":7,\"t\":\"drive\",\"v\":10,\"w\":-3}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDrive), K(c.kind));
  TEST_ASSERT_EQUAL(10, c.v);
  TEST_ASSERT_EQUAL(-3, c.w);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"drive\",\"\":1,\"v\":10}").kind));  // "" is not w
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_lock\",\"on\":1}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_joy\",\"x\":1,\"y\":2,\"active\":\"yes\"}").kind));
  const WsCommand j = P("{\"t\":\"cam_joy\",\"x\":1,\"y\":2,\"active\":false}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamJoy), K(j.kind));
  TEST_ASSERT_FALSE(j.active);
}

// Bridge LLD R5: opt-in debug stream - "on" required, hz 10 (default) or 50.
void test_debug_command() {
  WsCommand c = P("{\"t\":\"debug\",\"on\":true,\"hz\":50,\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDebug), K(c.kind));
  TEST_ASSERT_TRUE(c.on);
  TEST_ASSERT_EQUAL_UINT8(50, c.debug_hz);
  TEST_ASSERT_TRUE(c.has_id);
  c = P("{\"t\":\"debug\",\"on\":true}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDebug), K(c.kind));
  TEST_ASSERT_EQUAL_UINT8(10, c.debug_hz);  // default
  c = P("{\"t\":\"debug\",\"on\":false}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDebug), K(c.kind));
  TEST_ASSERT_FALSE(c.on);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"debug\",\"on\":true,\"hz\":20}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"debug\",\"hz\":10}").kind));  // on required
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"debug\",\"on\":true,\"hz\":\"x\"}").kind));
}

// Plan K (bridge LLD R7.1): move and calib.
void test_move_fields_and_ranges() {
  WsCommand c = P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":7}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(c.kind));
  TEST_ASSERT_EQUAL_INT16(100, c.move_cm);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);
  TEST_ASSERT_EQUAL_INT32(7, c.id);
  c = P("{\"t\":\"move\",\"dist_cm\":-50}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(c.kind));
  TEST_ASSERT_EQUAL_INT16(-50, c.move_cm);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);  // default
  TEST_ASSERT_EQUAL_INT16(500, P("{\"t\":\"move\",\"dist_cm\":500}").move_cm);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(P("{\"t\":\"move\",\"dist_cm\":1,\"speed\":100}").kind));
  const char* bad[] = {
      "{\"t\":\"move\"}",                                  // no distance
      "{\"t\":\"move\",\"dist_cm\":0}",                    // zero
      "{\"t\":\"move\",\"dist_cm\":501}",                  // too far
      "{\"t\":\"move\",\"dist_cm\":-501}",
      "{\"t\":\"move\",\"dist_cm\":10.5}",                 // whole cm only
      "{\"t\":\"move\",\"dist_cm\":10,\"speed\":9}",       // below 10 %
      "{\"t\":\"move\",\"dist_cm\":10,\"speed\":101}",
      "{\"t\":\"move\",\"dist_cm\":10,\"speed\":40.5}",
      "{\"t\":\"move\",\"dist_cm\":10,\"heading\":90}",    // not supported in Plan K
  };
  for (const char* s : bad) TEST_ASSERT_EQUAL_MESSAGE(K(WsCmdKind::kInvalid), K(P(s).kind), s);
}

void test_speed_limits_per_command() {  // Review Focus 5
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(P("{\"t\":\"move\",\"dist_cm\":10,\"speed\":10}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":10}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":12}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(P("{\"speed\":12,\"by\":90,\"t\":\"turn\"}").kind));  // any key order
}

void test_calib_read_and_set() {
  WsCommand c = P("{\"t\":\"calib\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(c.kind));
  TEST_ASSERT_FALSE(c.has_cpm);
  c = P("{\"t\":\"calib\",\"counts_per_m\":7480,\"id\":3}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(c.kind));
  TEST_ASSERT_TRUE(c.has_cpm);
  TEST_ASSERT_EQUAL_UINT16(7480, c.cpm);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"counts_per_m\":3000}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"counts_per_m\":15000}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":2999}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":15001}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":7480.5}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":\"x\"}").kind));
}

// Plan L (bridge LLD R8.1): pivot.
void test_turn_pivot_field() {
  WsCommand c = P("{\"t\":\"turn\",\"by\":90,\"pivot\":true}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(c.kind));
  TEST_ASSERT_TRUE(c.pivot);
  TEST_ASSERT_FALSE(P("{\"t\":\"turn\",\"by\":90}").pivot);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"pivot\":true,\"radius_cm\":60}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"pivot\":1}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(P("{\"t\":\"turn\",\"by\":90,\"pivot\":false,\"radius_cm\":60}").kind));
}

// Plan M (bridge LLD R9 / spec 02 §I3).
void test_wifi_commands() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiStatus), K(P("{\"t\":\"wifi_status\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiScan), K(P("{\"t\":\"wifi_scan\"}").kind));
  WsCommand c = P("{\"t\":\"wifi_save\",\"slot\":2,\"ssid\":\"Office\",\"password\":\"secret123\",\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiSave), K(c.kind));
  TEST_ASSERT_EQUAL_INT8(1, c.slot);  // JSON 1..3 -> 0..2
  TEST_ASSERT_EQUAL_STRING("Office", c.ssid);
  TEST_ASSERT_EQUAL_STRING("secret123", c.password);
  c = P("{\"t\":\"wifi_save\",\"ssid\":\"Open\",\"password\":\"\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiSave), K(c.kind));
  TEST_ASSERT_EQUAL_INT8(-1, c.slot);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"wifi_save\",\"password\":\"x\"}").kind));  // no ssid
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"wifi_save\",\"ssid\":\"A\",\"slot\":4,\"password\":\"\"}").kind));
  c = P("{\"t\":\"wifi_forget\",\"slot\":1}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiForget), K(c.kind));
  TEST_ASSERT_EQUAL_INT8(0, c.slot);
  TEST_ASSERT_EQUAL_INT8(-1, P("{\"t\":\"wifi_forget\"}").slot);  // all
  TEST_ASSERT_TRUE(P("{\"t\":\"wifi_mode\",\"mode\":\"ap\"}").wifi_ap);
  TEST_ASSERT_FALSE(P("{\"t\":\"wifi_mode\",\"mode\":\"home\"}").wifi_ap);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"wifi_mode\",\"mode\":\"x\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kApPassword), K(P("{\"t\":\"ap_password\",\"password\":\"abcdefgh\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kRestart), K(P("{\"t\":\"restart\"}").kind));
}

void test_setting_and_calib_reset() {
  WsCommand c = P("{\"t\":\"setting\",\"setting_id\":66}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSetting), K(c.kind));
  TEST_ASSERT_EQUAL_UINT8(66, c.setting_id);
  TEST_ASSERT_FALSE(c.has_value);
  c = P("{\"t\":\"setting\",\"setting_id\":66,\"value\":-12}");
  TEST_ASSERT_TRUE(c.has_value);
  TEST_ASSERT_EQUAL_INT32(-12, c.value);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"setting\"}").kind));
  TEST_ASSERT_EQUAL_INT8(0, P("{\"t\":\"calib\",\"reset\":\"counts_per_m\"}").calib_reset);
  TEST_ASSERT_EQUAL_INT8(1, P("{\"t\":\"calib\",\"reset\":\"roll\"}").calib_reset);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"reset\":\"x\"}").kind));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_drive);
  RUN_TEST(test_drive_clamps_and_rounds);
  RUN_TEST(test_drive_needs_v_and_w);
  RUN_TEST(test_stop_with_id);
  RUN_TEST(test_cam_joy);
  RUN_TEST(test_simple_kinds);
  RUN_TEST(test_hello_ignores_key_and_nested);
  RUN_TEST(test_ping_echoes_t0_verbatim);
  RUN_TEST(test_id_out_of_range_is_invalid);
  RUN_TEST(test_ping_t0_is_valid_json_number);
  RUN_TEST(test_long_stop_is_still_a_stop);
  RUN_TEST(test_unsupported_commands);
  RUN_TEST(test_junk_is_invalid);
  RUN_TEST(test_length_is_respected);
  RUN_TEST(test_sound_by_name);
  RUN_TEST(test_sound_unknown_name_invalid);
  RUN_TEST(test_cam_lock);
  RUN_TEST(test_audio_mode_parse);
  RUN_TEST(test_turn_by_and_to);
  RUN_TEST(test_turn_invalid);
  RUN_TEST(test_drive_rate_and_cancel);
  RUN_TEST(test_odd_keys_and_bools);
  RUN_TEST(test_debug_command);
  RUN_TEST(test_move_fields_and_ranges);
  RUN_TEST(test_speed_limits_per_command);
  RUN_TEST(test_calib_read_and_set);
  RUN_TEST(test_turn_pivot_field);
  RUN_TEST(test_wifi_commands);
  RUN_TEST(test_setting_and_calib_reset);
  return UNITY_END();
}
