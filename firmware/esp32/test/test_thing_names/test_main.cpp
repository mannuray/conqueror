// Plan Z2a Task 2: loose names for things (Tank Logo LLD §24.1).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "logo/thing_names.h"

using namespace cq::logo;

void setUp() {}
void tearDown() {}

namespace {
const char* const kWords[] = {"toy", "ball"};
const uint8_t kN[] = {2, 1};
const char* const kNames[] = {"car", "Racing  Car", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              "ball", nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
bool setBasic(ThingNames& t) { return t.set(kWords, kN, kNames, 2); }
}  // namespace

void test_resolve_word_name_case_and_spaces() {
  ThingNames t;
  TEST_ASSERT_TRUE(setBasic(t));
  TEST_ASSERT_EQUAL(2, t.size());
  TEST_ASSERT_EQUAL_STRING("toy", t.resolve("toy"));
  TEST_ASSERT_EQUAL_STRING("toy", t.resolve("CAR"));
  TEST_ASSERT_EQUAL_STRING("toy", t.resolve("  racing   car "));
  TEST_ASSERT_EQUAL_STRING("ball", t.resolve("Ball"));
  TEST_ASSERT_NULL(t.resolve("dragon"));
  TEST_ASSERT_NULL(t.resolve(""));
  TEST_ASSERT_NULL(t.resolve(nullptr));
  t.clear();
  TEST_ASSERT_NULL(t.resolve("toy"));
}

void test_bounds_refused_old_kept() {
  ThingNames t;
  TEST_ASSERT_TRUE(setBasic(t));
  static const char* many_words[49];
  static uint8_t many_n[49];
  static const char* many_names[49 * ThingNames::kMaxNames];
  static char buf[49][8];
  for (int i = 0; i < 49; ++i) {
    snprintf(buf[i], sizeof buf[i], "w%d", i);
    many_words[i] = buf[i];
    many_n[i] = 0;
  }
  TEST_ASSERT_FALSE(t.set(many_words, many_n, many_names, 49));
  TEST_ASSERT_TRUE(t.set(many_words, many_n, many_names, 48));  // exactly 48 is fine
  TEST_ASSERT_EQUAL(48, t.size());
  TEST_ASSERT_NULL(t.resolve("toy"));
  TEST_ASSERT_TRUE(setBasic(t));
  many_n[3] = 9;  // nine names for one word
  TEST_ASSERT_FALSE(t.set(many_words, many_n, many_names, 48));
  TEST_ASSERT_EQUAL_STRING("toy", t.resolve("car"));  // old list kept
  TEST_ASSERT_EQUAL(2, t.size());
}

void test_name_over_24_bytes_refused() {
  ThingNames t;
  TEST_ASSERT_TRUE(setBasic(t));
  const char* const w[] = {"thing"};
  const uint8_t n[] = {1};
  const char* ok24[8] = {"abcdefghijklmnopqrstuvwx"};  // 24 bytes
  const char* long25[8] = {"abcdefghijklmnopqrstuvwxy"};
  TEST_ASSERT_FALSE(t.set(w, n, long25, 1));
  TEST_ASSERT_EQUAL_STRING("toy", t.resolve("car"));  // old list kept
  TEST_ASSERT_TRUE(t.set(w, n, ok24, 1));
  TEST_ASSERT_EQUAL_STRING("thing", t.resolve("ABCDEFGHIJKLMNOPQRSTUVWX"));
  const char* const longword[] = {"abcdefghijklmnopqrstuvwxy"};
  const char* none[8] = {};
  const uint8_t zero[] = {0};
  TEST_ASSERT_FALSE(t.set(longword, zero, none, 1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_resolve_word_name_case_and_spaces);
  RUN_TEST(test_bounds_refused_old_kept);
  RUN_TEST(test_name_over_24_bytes_refused);
  return UNITY_END();
}
