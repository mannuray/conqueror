// Plan N1 Task 2: messages from templates, sentence dropping, dedupe, first error by position.
#include <string.h>
#include <unity.h>

#include "logo/diag.h"

using namespace cq::logo;

void setUp() {}
void tearDown() {}

void test_render_fills_placeholders() {
  char out[192];
  const DiagArg a[] = {{"word", "FORWARD"}, {"what", "a distance"}, {"example", "FORWARD 50"}};
  renderMessage("{word} needs {what}, like {example}.", a, 3, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("FORWARD needs a distance, like FORWARD 50.", out);
}

void test_render_drops_the_sentence_with_an_empty_value() {
  char out[192];
  const DiagArg a[] = {{"word", "SQAURE"}, {"suggestion", ""}};
  renderMessage("I don't know how to {word}. Did you mean {suggestion}?", a, 2, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("I don't know how to SQAURE.", out);
}

void test_add_renders_from_the_generated_table_and_dedupes() {
  Diagnostics d;
  const DiagArg a[] = {{"value", "-5"}};
  d.add(7004, 1, 7, a, 1);
  d.add(7004, 1, 7, a, 1);
  TEST_ASSERT_EQUAL(1, d.count());
  TEST_ASSERT_EQUAL_STRING("You didn't say what to do with -5.", d.at(0).msg);
  d.appendToLast(" To subtract, write 10 - 5.");
  TEST_ASSERT_NOT_NULL(strstr(d.at(0).msg, "10 - 5"));
}

void test_first_is_earliest_error_not_warning() {
  Diagnostics d;
  d.add(7001, 3, 1);
  d.add(7019, 1, 1, nullptr, 0, true);
  d.add(7004, 2, 5);
  d.add(7016, 2, 5);
  TEST_ASSERT_EQUAL(3, d.errorCount());
  TEST_ASSERT_EQUAL(7004, d.first()->code);  // line 2 beats line 3; the warning never counts; ties keep order
}

void test_capacity_is_respected() {
  Diagnostics d;
  for (uint16_t i = 0; i < 40; ++i) d.add(7001, i + 1, 1);
  TEST_ASSERT_EQUAL(Diagnostics::kCap, d.count());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_fills_placeholders);
  RUN_TEST(test_render_drops_the_sentence_with_an_empty_value);
  RUN_TEST(test_add_renders_from_the_generated_table_and_dedupes);
  RUN_TEST(test_first_is_earliest_error_not_warning);
  RUN_TEST(test_capacity_is_respected);
  return UNITY_END();
}
