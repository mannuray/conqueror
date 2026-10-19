// Plan N3 Task 2: one check (read → check → compile → plan the path) and its JSON reply in chunks (LLD §20.2, §20.4).
#include <string.h>
#include <unity.h>

#include <string>

#include "logo/check_json.h"

using namespace cq::logo;

namespace {
CheckSpace g_s;
std::string g_json;
int g_chunks = 0;

bool sink(void*, const char* data, size_t len) {
  g_json.append(data, len);
  ++g_chunks;
  return true;
}

bool check(const char* src, bool encoders = true) {
  Caps caps;
  caps.encoders = encoders;
  const bool ok = checkProgram(src, static_cast<uint16_t>(strlen(src)), caps, g_s);
  g_json.clear();
  g_chunks = 0;
  JsonOut out(sink, nullptr);
  TEST_ASSERT_TRUE(writeCheckReply(g_s, src, ok, 17, out));
  return ok;
}
bool has(const char* needle) { return g_json.find(needle) != std::string::npos; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_a_good_program_has_its_path_and_number() {
  TEST_ASSERT_TRUE(check("REPEAT 4 [ FD 50 RT 90 ]"));
  TEST_ASSERT_TRUE(g_s.program.ok);
  TEST_ASSERT_EQUAL(9, g_s.path.n);
  TEST_ASSERT_TRUE(has("{\"ok\":true,\"prog\":17,\"diags\":[],\"words\":[],\"handles_obstacles\":false,"));
  TEST_ASSERT_TRUE(has("\"path\":[[0,0,0,0,0],[0,500,0,1,0],[0,500,90,1,0],"));
  TEST_ASSERT_TRUE(has("\"cut_short\":false,\"sim_error\":null}"));
}

void test_errors_are_listed_and_nothing_runs() {
  TEST_ASSERT_FALSE(check("FD 600\nFOO"));
  TEST_ASSERT_FALSE(g_s.compiled);
  TEST_ASSERT_TRUE(has("{\"ok\":false,\"prog\":17,\"diags\":[{"));  // the reader's errors, then the checker's
  TEST_ASSERT_TRUE(has("\"code\":7025,\"line\":1,\"col\":4,\"msg\":"));
  TEST_ASSERT_TRUE(has("\"code\":7001,\"line\":2,\"col\":1"));
  TEST_ASSERT_TRUE(has("\"path\":[]"));
}

void test_your_words_and_obstacles() {
  TEST_ASSERT_TRUE(check("to square :s repeat 4 [ fd :s rt 90 ] end\nTO SIDE OUTPUT 5 END\nsquare side IF BLOCKED? [ BEEP ]"));
  TEST_ASSERT_TRUE(has("\"words\":[{\"name\":\"SQUARE\",\"inputs\":1,\"gives\":false},{\"name\":\"SIDE\",\"inputs\":0,\"gives\":true}],"));
  TEST_ASSERT_TRUE(has("\"handles_obstacles\":true"));
}

void test_no_encoders_refuses_moves_before_running() {
  TEST_ASSERT_FALSE(check("FD 10", false));
  TEST_ASSERT_TRUE(has("\"code\":7021"));
  TEST_ASSERT_TRUE(check("RT 90 BEEP", false));
}

void test_too_long_is_7010() {
  static char big[8300];
  memset(big, ' ', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  TEST_ASSERT_FALSE(check(big));
  TEST_ASSERT_TRUE(has("\"code\":7010"));
}

void test_a_long_path_goes_out_in_chunks_and_is_cut_short() {
  TEST_ASSERT_TRUE(check("FOREVER [ FD 10 RT 1 ]"));
  TEST_ASSERT_TRUE(g_chunks > 20);
  TEST_ASSERT_TRUE(has("\"cut_short\":true"));
  TEST_ASSERT_EQUAL('{', g_json.front());
  TEST_ASSERT_EQUAL('}', g_json.back());
}

void test_a_run_time_error_in_the_plan() {
  TEST_ASSERT_TRUE(check("FD 10\nFD 5 / 0"));
  TEST_ASSERT_TRUE(has("\"sim_error\":{\"code\":7013,\"line\":2,\"msg\":\"Can't divide by zero on line 2.\"}"));
}

void test_text_is_escaped() {
  g_json.clear();
  JsonOut out(sink, nullptr);
  out.text("a\"b\\c\n", 6);
  out.finish();
  TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\u000a\"", g_json.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_good_program_has_its_path_and_number);
  RUN_TEST(test_errors_are_listed_and_nothing_runs);
  RUN_TEST(test_your_words_and_obstacles);
  RUN_TEST(test_no_encoders_refuses_moves_before_running);
  RUN_TEST(test_too_long_is_7010);
  RUN_TEST(test_a_long_path_goes_out_in_chunks_and_is_cut_short);
  RUN_TEST(test_a_run_time_error_in_the_plan);
  RUN_TEST(test_text_is_escaped);
  return UNITY_END();
}
