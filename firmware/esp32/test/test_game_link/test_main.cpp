// The game's messages with the engine (bridge LLD R34.3): its hello's "people", its person stream, game on / off,
// the back_ok question and a lease ended by a catch.
#include <string.h>
#include <unity.h>

#include "core/skill_hub.h"
#include "core/ws_codec.h"
#include "core/ws_json_skill.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

namespace {
WsCommand parse(const char* s) { return parseWsCommand(s, strlen(s)); }
}  // namespace

void test_hello_people_flag() {
  TEST_ASSERT_TRUE(parse("{\"t\":\"hello\",\"client\":\"ai-skills\",\"people\":true}").people);
  TEST_ASSERT_FALSE(parse("{\"t\":\"hello\",\"client\":\"ai-skills\",\"people\":false}").people);
  TEST_ASSERT_FALSE(parse("{\"t\":\"hello\",\"client\":\"ai-skills\"}").people);
  SkillHub hub;
  hub.onHello(7, "", 0, 0, true);
  TEST_ASSERT_TRUE(hub.people());
  hub.onClose(7);
  TEST_ASSERT_FALSE(hub.people());  // no engine: no seeing-test pass
  SkillHub none;
  TEST_ASSERT_FALSE(none.people());
}

void test_person_message_parsed() {
  const WsCommand c =
      parse("{\"t\":\"person\",\"seen\":true,\"d_cm\":85,\"bearing\":-20,\"coming\":1,\"sure\":0.5,\"age_ms\":40}");
  TEST_ASSERT_EQUAL(static_cast<int>(WsCmdKind::kPerson), static_cast<int>(c.kind));
  TEST_ASSERT_TRUE(c.person.seen);
  TEST_ASSERT_EQUAL_INT16(85, c.person.d_cm);
  TEST_ASSERT_EQUAL_INT16(-20, c.person.bearing_deg);
  TEST_ASSERT_EQUAL_INT8(1, c.person.coming);
  TEST_ASSERT_UINT8_WITHIN(1, 128, c.person.sure);
  TEST_ASSERT_EQUAL_UINT16(40, c.person_age_ms);
  const WsCommand none = parse("{\"t\":\"person\",\"seen\":false,\"d_cm\":-1,\"bearing\":0}");
  TEST_ASSERT_EQUAL(static_cast<int>(WsCmdKind::kPerson), static_cast<int>(none.kind));
  TEST_ASSERT_EQUAL_INT16(-1, none.person.d_cm);
}

void test_person_message_bounded() {
  const char* bad[] = {
      "{\"t\":\"person\",\"seen\":true,\"d_cm\":1001,\"bearing\":0}",   // over 10 m
      "{\"t\":\"person\",\"seen\":true,\"d_cm\":-2,\"bearing\":0}",     // only -1 means none
      "{\"t\":\"person\",\"seen\":true,\"d_cm\":50,\"bearing\":181}",   // -180..180
      "{\"t\":\"person\",\"seen\":true,\"d_cm\":50,\"bearing\":0,\"coming\":2}",
      "{\"t\":\"person\",\"seen\":true,\"d_cm\":50,\"bearing\":0,\"sure\":1.5}",
      "{\"t\":\"person\",\"seen\":true,\"bearing\":0}",                 // d_cm missing
      "{\"t\":\"person\",\"d_cm\":50,\"bearing\":0}",                   // seen missing
  };
  for (const char* s : bad) {
    const WsCommand c = parse(s);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(WsCmdKind::kBadField), static_cast<int>(c.kind), s);
  }
}

void test_person_only_from_the_server() {
  SkillHub hub;
  hub.onHello(7, "", 0, 0, true);
  PersonEst e;
  e.seen = true;
  e.d_cm = 90;
  TEST_ASSERT_FALSE(hub.onPerson(8, e, 40, 1000));  // another client: ignored
  PersonEst out;
  TEST_ASSERT_FALSE(hub.takePerson(out));
  TEST_ASSERT_TRUE(hub.onPerson(7, e, 40, 1000));
  TEST_ASSERT_TRUE(hub.takePerson(out));
  TEST_ASSERT_EQUAL_INT16(90, out.d_cm);
  TEST_ASSERT_EQUAL_UINT32(960, out.at_ms);  // received at 1000, 40 ms old
  TEST_ASSERT_FALSE(hub.takePerson(out));    // one-shot
}

void test_game_on_off_json() {
  char b[64];
  TEST_ASSERT_TRUE(jsonGame(b, sizeof b, true) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"game\",\"on\":true}", b);
  jsonGame(b, sizeof b, false);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"game\",\"on\":false}", b);
}

void test_back_ok_question_json() {
  char b[96];
  TEST_ASSERT_TRUE(jsonSkillAskBackOk(b, sizeof b, 12, 20) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"skill_ask\",\"id\":12,\"q\":\"back_ok\",\"cm\":20}", b);
}

void test_skill_end_caught() {
  char b[64];
  jsonSkillEnd(b, sizeof b, 5, LeaseEnd::kCaught);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"skill_end\",\"id\":5,\"why\":\"caught\"}", b);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hello_people_flag);
  RUN_TEST(test_person_message_parsed);
  RUN_TEST(test_person_message_bounded);
  RUN_TEST(test_person_only_from_the_server);
  RUN_TEST(test_game_on_off_json);
  RUN_TEST(test_back_ok_question_json);
  RUN_TEST(test_skill_end_caught);
  return UNITY_END();
}
