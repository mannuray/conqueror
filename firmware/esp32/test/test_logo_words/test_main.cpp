// Plan N1 Task 1: the generated word table compiles and carries the spec's facts.
#include <string.h>
#include <unity.h>

#include "logo/generated/words.h"

using namespace cq::logo::gen;

void setUp() {}
void tearDown() {}

void test_forward_entry() {
  const WordSpec* fd = nullptr;
  for (uint8_t i = 0; i < kWordCount; ++i) if (strcmp(kWords[i].name, "FORWARD") == 0) fd = &kWords[i];
  TEST_ASSERT_NOT_NULL(fd);
  TEST_ASSERT_EQUAL(1, fd->n_inputs);
  TEST_ASSERT_EQUAL(1000, fd->inputs[0].min_milli);
  TEST_ASSERT_EQUAL(500000, fd->inputs[0].max_milli);
  TEST_ASSERT_TRUE(fd->needs & kNeedEncoders);
}

void test_presets_and_errors() {
  TEST_ASSERT_EQUAL(24, kSoundsCount);
  TEST_ASSERT_EQUAL_STRING("OFF", kColours[kColoursCount - 1]);
  bool has7029 = false;
  for (uint8_t i = 0; i < kErrorCount; ++i) has7029 |= kErrors[i].code == 7029;
  TEST_ASSERT_TRUE(has7029);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_forward_entry);
  RUN_TEST(test_presets_and_errors);
  return UNITY_END();
}
