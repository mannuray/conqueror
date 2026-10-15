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
  TEST_ASSERT_EQUAL(K(WsCmdKind::kUnsupported), K(P("{\"t\":\"turn\",\"angle\":90}").kind));
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
  return UNITY_END();
}
