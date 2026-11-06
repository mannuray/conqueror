// Plan N3 Task 3: the checked program box (numbers, take refusals), program names, the built-in examples;
// Plan L1: the built-in lessons (Tank Logo LLD §25).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "logo/check_box.h"
#include "logo/check_job.h"
#include "logo/program_names.h"
#include "logo/thing_names.h"

using namespace cq::logo;

namespace {
Program g_checked, g_running, g_compiled;
CheckBox g_box(g_checked);
CheckSpace g_space;
}  // namespace

void setUp() {}
void tearDown() {}

void test_numbers_go_up_and_only_the_current_ok_program_runs() {
  g_compiled.clear();
  g_compiled.ok = true;
  g_compiled.n_code = 3;
  const uint32_t a = g_box.put(&g_compiled, "square");
  char name[CheckBox::kNameCap];
  TEST_ASSERT_EQUAL(CheckBox::Take::kOk, g_box.take(a, g_running, name, sizeof name));
  TEST_ASSERT_EQUAL(3, g_running.n_code);
  TEST_ASSERT_EQUAL_STRING("square", name);
  const uint32_t b = g_box.put(&g_compiled, "");
  TEST_ASSERT_EQUAL(a + 1, b);
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(a, g_running, name, sizeof name));  // replaced
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(0, g_running, name, sizeof name));
}

void test_a_check_with_errors_gets_a_number_but_never_runs() {
  const uint32_t n = g_box.put(nullptr, "");
  char name[CheckBox::kNameCap];
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(n, g_running, name, sizeof name));
  g_compiled.ok = false;
  const uint32_t m = g_box.put(&g_compiled, "");
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(m, g_running, name, sizeof name));
}

void test_program_names() {
  char out[25];
  TEST_ASSERT_TRUE(programName("My_Square-2", out, sizeof out));
  TEST_ASSERT_EQUAL_STRING("my_square-2", out);
  TEST_ASSERT_FALSE(programName("", out, sizeof out));
  TEST_ASSERT_FALSE(programName("has space", out, sizeof out));
  TEST_ASSERT_FALSE(programName("../etc", out, sizeof out));
  TEST_ASSERT_FALSE(programName("abcdefghijklmnopqrstuvwxy", out, sizeof out));  // 25
  TEST_ASSERT_TRUE(programName("abcdefghijklmnopqrstuvwx", out, sizeof out));    // 24
}

Caps withAiHelper(ThingNames& t) {
  static const char* const words[] = {"toy", "ball"};
  static const uint8_t n[] = {2, 1};
  static const char* const names[] = {"car", "racing car", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                      "ball", nullptr,     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  t.set(words, n, names, 2);
  Caps c;
  c.ai = true;
  c.things = &t;
  return c;
}

void test_every_built_in_checks_ok_with_the_ai_helper() {
  static const char* const kNames[] = {"back_to_start", "where_is_my_ball", "treasure_hunt", "hot_and_cold",
                                       "hot_and_cold_hunt", "first_square", "avoid_walls", "look_around"};
  TEST_ASSERT_EQUAL(8, kBuiltInCount);
  TEST_ASSERT_EQUAL(kBuiltInCount, exampleCount());
  ThingNames things;
  const Caps caps = withAiHelper(things);
  for (uint8_t i = 0; i < exampleCount(); ++i) {
    const Example& e = example(i);
    TEST_ASSERT_EQUAL_STRING(kNames[i], e.name);
    TEST_ASSERT_EQUAL_MESSAGE(i < 5, e.lesson, e.name);
    if (e.lesson) {
      char title[16];
      snprintf(title, sizeof title, "; Lesson %u", i + 1);
      TEST_ASSERT_EQUAL_MESSAGE(0, strncmp(e.src, title, strlen(title)), e.name);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.src, "; Try this:"), e.name);
    }
    char lower[25];
    TEST_ASSERT_TRUE_MESSAGE(programName(e.name, lower, sizeof lower), e.name);
    const bool ok = checkProgram(e.src, static_cast<uint16_t>(strlen(e.src)), caps, g_space);
    char why[64];
    const Diag* d = g_space.diags.first();
    snprintf(why, sizeof why, "%s: %u at line %u col %u", e.name, d ? d->code : 0, d ? d->line : 0, d ? d->col : 0);
    TEST_ASSERT_TRUE_MESSAGE(ok, why);
  }
  TEST_ASSERT_NOT_NULL(findExample("treasure_hunt"));
  TEST_ASSERT_NULL(findExample("nothing"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_numbers_go_up_and_only_the_current_ok_program_runs);
  RUN_TEST(test_a_check_with_errors_gets_a_number_but_never_runs);
  RUN_TEST(test_program_names);
  RUN_TEST(test_every_built_in_checks_ok_with_the_ai_helper);
  return UNITY_END();
}
