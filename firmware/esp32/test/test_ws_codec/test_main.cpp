// WsCodec: the Basic Drive JSON slice (bridge LLD R2.9) in, robust against junk.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "core/thing_list.h"
#include "core/ws_codec.h"
#include "logo/run_controller.h"

using cq::core::parseWsCommand;
using cq::core::WsCmdKind;
using cq::core::WsCommand;

namespace {
WsCommand P(const char* s) { return parseWsCommand(s, strlen(s)); }
int K(WsCmdKind k) { return static_cast<int>(k); }
int K(cq::core::ThingParse k) { return static_cast<int>(k); }
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
  TEST_ASSERT_EQUAL_UINT8(24, P("{\"t\":\"sound\",\"name\":\"horn\"}").sound_id);  // design-notes §75
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

// R19: the gyro scale - set (9000..11000, whole numbers) and reset.
void test_calib_gyro_scale() {
  WsCommand c = P("{\"t\":\"calib\",\"gyro_scale\":10230,\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(c.kind));
  TEST_ASSERT_TRUE(c.has_gyro_scale);
  TEST_ASSERT_FALSE(c.has_cpm);
  TEST_ASSERT_EQUAL_UINT16(10230, c.gyro_scale);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"gyro_scale\":9000}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"gyro_scale\":11000}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"gyro_scale\":8999}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"gyro_scale\":11001}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"gyro_scale\":10230.5}").kind));
  TEST_ASSERT_EQUAL_INT8(2, P("{\"t\":\"calib\",\"reset\":\"gyro_scale\"}").calib_reset);
}

// Bridge LLD R22: cam_set - degrees from the centre (right / up = +), 0.1 deg steps -> the Uno's servo values
// (900 - deg x 10); outside pan -90..90 / tilt -20..60 it is still a cam_set, marked out of range (its own nack).
void test_cam_set() {
  WsCommand c = P("{\"t\":\"cam_set\",\"pan\":15,\"tilt\":-15,\"id\":7}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamSet), K(c.kind));
  TEST_ASSERT_TRUE(c.cam_in_range);
  TEST_ASSERT_EQUAL_INT16(750, c.cam_pan_dd);
  TEST_ASSERT_EQUAL_INT16(1050, c.cam_tilt_dd);
  c = P("{\"t\":\"cam_set\",\"pan\":-90,\"tilt\":60}");
  TEST_ASSERT_TRUE(c.cam_in_range);
  TEST_ASSERT_EQUAL_INT16(1800, c.cam_pan_dd);
  TEST_ASSERT_EQUAL_INT16(300, c.cam_tilt_dd);
  c = P("{\"t\":\"cam_set\",\"pan\":0.2,\"tilt\":-20}");
  TEST_ASSERT_EQUAL_INT16(898, c.cam_pan_dd);  // 0.1 deg steps
  TEST_ASSERT_EQUAL_INT16(1100, c.cam_tilt_dd);
  const char* outside[] = {"{\"t\":\"cam_set\",\"pan\":91,\"tilt\":0}", "{\"t\":\"cam_set\",\"pan\":0,\"tilt\":61}",
                           "{\"t\":\"cam_set\",\"pan\":0,\"tilt\":-21}", "{\"t\":\"cam_set\",\"pan\":-90.1,\"tilt\":0}"};
  for (const char* s : outside) {
    c = P(s);
    TEST_ASSERT_EQUAL_MESSAGE(K(WsCmdKind::kCamSet), K(c.kind), s);
    TEST_ASSERT_FALSE_MESSAGE(c.cam_in_range, s);
  }
  c = P("{\"t\":\"cam_set\",\"pan\":5000,\"tilt\":0}");  // final review: any number outside is OUT_OF_RANGE
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamSet), K(c.kind));
  TEST_ASSERT_FALSE(c.cam_in_range);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_set\",\"pan\":10}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_set\",\"pan\":\"x\",\"tilt\":0}").kind));
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
  TEST_ASSERT_FALSE(c.try_only);  // R23: a try changes the Uno's RAM only
  TEST_ASSERT_TRUE(P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-50,\"try\":true}").try_only);
  TEST_ASSERT_EQUAL_INT8(0, P("{\"t\":\"calib\",\"reset\":\"counts_per_m\"}").calib_reset);
  TEST_ASSERT_EQUAL_INT8(1, P("{\"t\":\"calib\",\"reset\":\"roll\"}").calib_reset);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"reset\":\"x\"}").kind));
}


// Plan O (bridge LLD R13): tank, log, selftest, camera_set, app_set.
void test_tank_log_selftest() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTank), K(P("{\"t\":\"tank\"}").kind));
  const WsCommand l = P("{\"t\":\"log\",\"since\":57}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kLog), K(l.kind));
  TEST_ASSERT_EQUAL_UINT32(57, l.log_since);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kLog), K(P("{\"t\":\"log\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSelfTest), K(P("{\"t\":\"selftest\",\"id\":3}").kind));
}

void test_camera_set_false_turns_a_switch_off() {  // tank check: "flip upside down is always on"
  const WsCommand c = P("{\"t\":\"camera_set\",\"vflip\":false,\"id\":5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCameraSet), K(c.kind));
  TEST_ASSERT_TRUE(c.cam.has_vflip);
  TEST_ASSERT_FALSE(c.cam.vflip);
  const WsCommand d = P("{\"t\":\"camera_set\",\"low_light\":false,\"hmirror\":false}");
  TEST_ASSERT_TRUE(d.cam.has_low && !d.cam.low && d.cam.has_hmirror && !d.cam.hmirror);
}

void test_camera_set_white_balance_and_night() {  // R24
  const WsCommand c = P("{\"t\":\"camera_set\",\"wb\":\"home\",\"night\":true,\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCameraSet), K(c.kind));
  TEST_ASSERT_TRUE(c.cam.has_wb && c.cam.wb == 4 && c.cam.has_night && c.cam.night);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"wb\":\"disco\"}").kind));
}

void test_camera_set_fields_and_ranges() {
  const WsCommand c = P("{\"t\":\"camera_set\",\"size\":\"SVGA\",\"quality\":20,\"fps_cap\":10,\"brightness\":-1,"
                        "\"low_light\":true,\"hmirror\":false,\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCameraSet), K(c.kind));
  TEST_ASSERT_TRUE(c.cam.has_size);
  TEST_ASSERT_EQUAL(2, c.cam.size);
  TEST_ASSERT_EQUAL(20, c.cam.quality);
  TEST_ASSERT_EQUAL(10, c.cam.fps);
  TEST_ASSERT_EQUAL(-1, c.cam.bright);
  TEST_ASSERT_TRUE(c.cam.has_low && c.cam.low);
  TEST_ASSERT_TRUE(c.cam.has_hmirror && !c.cam.hmirror);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"quality\":5}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"size\":\"UXGA\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"fps_cap\":7}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\"}").kind));
  TEST_ASSERT_TRUE(P("{\"t\":\"camera_set\",\"reset\":true}").cam.reset);
  TEST_ASSERT_EQUAL(0, P("{\"t\":\"calib\",\"reset\":\"counts_per_m\"}").calib_reset);  // calib keeps its string reset
}

void test_app_url_too_long_is_refused_by_the_parser() {  // Plan O final review I3: never stored cut short
  char js[300];
  char url[201];
  memset(url, 'a', sizeof(url) - 1);
  memcpy(url, "http://", 7);
  url[200] = '\0';
  snprintf(js, sizeof(js), "{\"t\":\"app_set\",\"ai_url\":\"%s\"}", url);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P(js).kind));
  url[96] = '\0';  // exactly 96 characters is fine
  snprintf(js, sizeof(js), "{\"t\":\"app_set\",\"ai_url\":\"%s\"}", url);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAppSet), K(P(js).kind));
}

void test_app_url_validation() {
  const WsCommand a = P("{\"t\":\"app_set\",\"ai_url\":\"http://192.168.31.8:8000/\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAppSet), K(a.kind));
  TEST_ASSERT_EQUAL_STRING("http://192.168.31.8:8000/", a.ai_url);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAppSet), K(P("{\"t\":\"app_set\",\"ai_url\":\"\"}").kind));  // clears it
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"app_set\",\"ai_url\":\"javascript:x\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"app_set\"}").kind));
  TEST_ASSERT_FALSE(cq::core::appUrlValid("javascript:alert(1)"));
  TEST_ASSERT_FALSE(cq::core::appUrlValid("http://a b"));
  TEST_ASSERT_FALSE(cq::core::appUrlValid("http://x\"y"));
  TEST_ASSERT_FALSE(cq::core::appUrlValid("http://x<y"));
  char longu[220] = "http://";
  for (int i = 7; i < 210; ++i) longu[i] = 'a';
  longu[210] = '\0';
  TEST_ASSERT_FALSE(cq::core::appUrlValid(longu));
  TEST_ASSERT_TRUE(cq::core::appUrlValid("http://laptop.local:8000/ai"));
  TEST_ASSERT_TRUE(cq::core::appUrlValid(""));
}

// Plan N3: the logo_* commands (Tank Logo LLD §20.4).
void test_logo_commands() {
  WsCommand c = P("{\"t\":\"logo_run\",\"prog\":17,\"step\":true,\"id\":5}");
  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kLogoRun, c.kind);
  TEST_ASSERT_EQUAL(17, c.prog);
  TEST_ASSERT_TRUE(c.step);
  TEST_ASSERT_EQUAL(5, c.id);
  c = P("{\"t\":\"logo_run\",\"prog\":4294967295}");
  TEST_ASSERT_EQUAL(4294967295u, c.prog);
  TEST_ASSERT_FALSE(c.step);
  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kInvalid, P("{\"t\":\"logo_run\"}").kind);  // no program number
  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kInvalid, P("{\"t\":\"logo_run\",\"prog\":0}").kind);
  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kInvalid, P("{\"t\":\"logo_run\",\"prog\":1.5}").kind);
  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kLogoStep, P("{\"t\":\"logo_step\",\"id\":6}").kind);
  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kLogoStop, P("{\"t\":\"logo_stop\"}").kind);
}

// ---- Plan Z2a (bridge LLD R26.2): the skill server's messages ----
void test_hello_ai_skills() {
  const WsCommand c = P("{\"t\":\"hello\",\"client\":\"ai-skills\",\"proto\":1}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kHello), K(c.kind));
  TEST_ASSERT_EQUAL_STRING("ai-skills", c.client);
  TEST_ASSERT_FALSE(c.has_things);
}

namespace {
cq::core::ThingParse things(const char* json, cq::core::ThingList& out) {
  static char buf[4096];
  snprintf(buf, sizeof buf, "%s", json);
  return cq::core::parseThingList(buf, strlen(buf), out);
}
}  // namespace

void test_thing_list_layout_is_flat_with_stride_8() {
  static cq::core::ThingList l;
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kOk),
                    K(things("{\"t\":\"hello\",\"client\":\"ai-skills\",\"things\":[{\"word\":\"toy\",\"names\":[\"car\",\"racing car\"]},"
                             "{\"extra\":[1,{\"a\":2}],\"names\":[],\"word\":\"ball\"},{\"word\":\"cup\"}],\"x\":1}", l)));
  TEST_ASSERT_EQUAL(3, l.n);
  TEST_ASSERT_EQUAL_STRING("toy", l.word[0]);
  TEST_ASSERT_EQUAL(2, l.n_names[0]);
  TEST_ASSERT_EQUAL_STRING("car", l.names[0]);
  TEST_ASSERT_EQUAL_STRING("racing car", l.names[1]);
  TEST_ASSERT_EQUAL_STRING("ball", l.word[1]);
  TEST_ASSERT_EQUAL(0, l.n_names[1]);
  TEST_ASSERT_EQUAL_STRING("cup", l.word[2]);
  TEST_ASSERT_EQUAL(0, l.n_names[2]);
}

void test_thing_list_absent_empty_and_bad() {
  static cq::core::ThingList l;
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kOk), K(things("{\"t\":\"hello\"}", l)));
  TEST_ASSERT_EQUAL(0, l.n);
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kOk), K(things("{\"things\":[]}", l)));
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kBad), K(things("{\"things\":{}}", l)));
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kBad), K(things("{\"things\":[{\"names\":[\"a\"]}]}", l)));         // no word
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kBad), K(things("{\"things\":[{\"word\":5}]}", l)));
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kBad), K(things("{\"things\":[{\"word\":\"a\",\"names\":[3]}]}", l)));
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kBad), K(things("{\"things\":[{\"word\":\"a\\\"b\"}]}", l)));      // escape
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kBad), K(things("{\"things\":[{\"word\":\"a\"}", l)));            // cut
  TEST_ASSERT_EQUAL(0, l.n);  // a refused list leaves nothing half-filled
}

void test_thing_list_over_bounds_is_too_big() {
  static cq::core::ThingList l;
  char j[2048] = "{\"things\":[";
  for (int i = 0; i < 49; ++i) snprintf(j + strlen(j), sizeof j - strlen(j), "%s{\"word\":\"w%d\"}", i ? "," : "", i);
  strcat(j, "]}");
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kTooBig), K(things(j, l)));
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kTooBig),
                    K(things("{\"things\":[{\"word\":\"a\",\"names\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}]}", l)));
  TEST_ASSERT_EQUAL(K(cq::core::ThingParse::kOk),
                    K(things("{\"things\":[{\"word\":\"a\",\"names\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]}]}", l)));
  TEST_ASSERT_EQUAL(8, l.n_names[0]);
}

void test_skill_things_in_parts() {
  const WsCommand c = P("{\"t\":\"skill_things\",\"part\":2,\"of\":3,\"things\":[]}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillThings), K(c.kind));
  TEST_ASSERT_EQUAL(2, c.part);
  TEST_ASSERT_EQUAL(3, c.of);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillThings), K(P("{\"t\":\"skill_things\",\"part\":48,\"of\":48,\"things\":[]}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_things\",\"things\":[]}").kind));  // no part / of
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_things\",\"part\":1,\"of\":1}").kind));  // no things
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_things\",\"part\":0,\"of\":1,\"things\":[]}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_things\",\"part\":3,\"of\":2,\"things\":[]}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_things\",\"part\":1,\"of\":49,\"things\":[]}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_things\",\"part\":1.5,\"of\":2,\"things\":[]}").kind));
}

void test_a_bad_field_is_bad_field_whatever_the_key_order() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"id\":\"x\",\"t\":\"skill_done\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"v\":\"x\",\"id\":1,\"yaw_rate\":1,\"t\":\"skill_drive\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"id\":1,\"t\":\"skill_failed\",\"reason\":\"x\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"id\":\"x\",\"t\":\"drive\"}").kind));  // other kinds are unchanged
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"id\":\"x\"}").kind));
}

// Plan Z2b-1: a teach started between the check and the word.
void test_skill_failed_learning() {
  const WsCommand c = P("{\"t\":\"skill_failed\",\"id\":4,\"reason\":\"learning\",\"thing\":\"ball\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillFailed), K(c.kind));
  TEST_ASSERT_TRUE(c.skill_end == cq::logo::SkillEnd::kLearning);
}

void test_skill_answer_bool_and_cm() {
  WsCommand c = P("{\"t\":\"skill_answer\",\"id\":7,\"value\":true}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillAnswer), K(c.kind));
  TEST_ASSERT_EQUAL(7, c.id);
  TEST_ASSERT_TRUE(c.value_bool);
  TEST_ASSERT_EQUAL(1, c.value);
  c = P("{\"t\":\"skill_answer\",\"id\":7,\"value\":false}");
  TEST_ASSERT_EQUAL(0, c.value);
  c = P("{\"t\":\"skill_answer\",\"id\":8,\"value\":-1}");
  TEST_ASSERT_FALSE(c.value_bool);
  TEST_ASSERT_EQUAL(-1, c.value);
  c = P("{\"t\":\"skill_answer\",\"id\":8,\"value\":120}");
  TEST_ASSERT_EQUAL(120, c.value);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_answer\",\"id\":7}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_answer\",\"value\":3}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_answer\",\"id\":7,\"value\":\"x\"}").kind));
}

void test_a_bool_value_is_not_a_setting() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"setting\",\"setting_id\":3,\"value\":true}").kind));
}

void test_skill_drive() {
  const WsCommand c = P("{\"t\":\"skill_drive\",\"id\":9,\"v\":50,\"yaw_rate\":-12.5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillDrive), K(c.kind));
  TEST_ASSERT_EQUAL(9, c.id);
  TEST_ASSERT_EQUAL(50, c.v);
  TEST_ASSERT_EQUAL(-125, c.rate_ddps);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_drive\",\"v\":50,\"yaw_rate\":1}").kind));       // no id
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_drive\",\"id\":9,\"yaw_rate\":1}").kind));       // no v
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_drive\",\"id\":9,\"v\":5,\"yaw_rate\":999}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_drive\",\"id\":-1,\"v\":5,\"yaw_rate\":1}").kind));
}

void test_skill_done_and_failed() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillDone), K(P("{\"t\":\"skill_done\",\"id\":4}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_done\"}").kind));
  const char* const reasons[] = {"not_found", "lost", "outside_area", "no_way", "blocked", "unknown_thing", "stale"};
  using E = cq::logo::SkillEnd;
  const E kEnds[] = {E::kNotFound, E::kLost, E::kOutsideArea, E::kNoWay, E::kBlocked, E::kUnknownThing, E::kStale};
  for (int i = 0; i < 7; ++i) {
    char j[96];
    snprintf(j, sizeof j, "{\"t\":\"skill_failed\",\"id\":4,\"reason\":\"%s\",\"thing\":\"toy\"}", reasons[i]);
    const WsCommand c = P(j);
    TEST_ASSERT_EQUAL(K(WsCmdKind::kSkillFailed), K(c.kind));
    TEST_ASSERT_TRUE(c.skill_end == kEnds[i]);
    TEST_ASSERT_EQUAL_STRING("toy", c.thing);
  }
  TEST_ASSERT_EQUAL_STRING("", P("{\"t\":\"skill_failed\",\"id\":4,\"reason\":\"lost\"}").thing);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_failed\",\"id\":4,\"reason\":\"oops\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_failed\",\"id\":4}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField), K(P("{\"t\":\"skill_failed\",\"reason\":\"lost\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kBadField),  // a thing cut short is never taken as another word
                    K(P("{\"t\":\"skill_failed\",\"id\":4,\"reason\":\"lost\",\"thing\":\"abcdefghijklmnopqrstuvwxyz\"}").kind));
}

void test_skill_messages_sent_by_the_bridge_are_unsupported_not_bad() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kUnsupported), K(P("{\"t\":\"skill_ask\",\"id\":1}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("not json").kind));
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
  RUN_TEST(test_cam_set);
  RUN_TEST(test_calib_gyro_scale);
  RUN_TEST(test_tank_log_selftest);
  RUN_TEST(test_camera_set_fields_and_ranges);
  RUN_TEST(test_camera_set_white_balance_and_night);
  RUN_TEST(test_camera_set_false_turns_a_switch_off);
  RUN_TEST(test_app_url_validation);
  RUN_TEST(test_app_url_too_long_is_refused_by_the_parser);
  RUN_TEST(test_logo_commands);
  RUN_TEST(test_hello_ai_skills);
  RUN_TEST(test_thing_list_layout_is_flat_with_stride_8);
  RUN_TEST(test_thing_list_absent_empty_and_bad);
  RUN_TEST(test_thing_list_over_bounds_is_too_big);
  RUN_TEST(test_skill_things_in_parts);
  RUN_TEST(test_a_bad_field_is_bad_field_whatever_the_key_order);
  RUN_TEST(test_skill_answer_bool_and_cm);
  RUN_TEST(test_a_bool_value_is_not_a_setting);
  RUN_TEST(test_skill_drive);
  RUN_TEST(test_skill_done_and_failed);
  RUN_TEST(test_skill_failed_learning);
  RUN_TEST(test_skill_messages_sent_by_the_bridge_are_unsupported_not_bad);
  return UNITY_END();
}
