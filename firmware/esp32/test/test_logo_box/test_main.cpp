// Plan N3 Task 3: the checked program box (numbers, take refusals), program names, the built-in examples.
#include <string.h>
#include <unity.h>

#include "logo/check_box.h"
#include "logo/check_job.h"
#include "logo/program_names.h"

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

void test_every_example_checks_ok() {
  TEST_ASSERT_EQUAL(3, exampleCount());
  for (uint8_t i = 0; i < exampleCount(); ++i) {
    const Example& e = example(i);
    char lower[25];
    TEST_ASSERT_TRUE_MESSAGE(programName(e.name, lower, sizeof lower), e.name);
    Caps caps;
    TEST_ASSERT_TRUE_MESSAGE(checkProgram(e.src, static_cast<uint16_t>(strlen(e.src)), caps, g_space), e.name);
  }
  TEST_ASSERT_NOT_NULL(findExample("avoid_walls"));
  TEST_ASSERT_NULL(findExample("nothing"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_numbers_go_up_and_only_the_current_ok_program_runs);
  RUN_TEST(test_a_check_with_errors_gets_a_number_but_never_runs);
  RUN_TEST(test_program_names);
  RUN_TEST(test_every_example_checks_ok);
  return UNITY_END();
}
