// Plan L (bridge LLD R8): arc speed band, minimum radius, refusal text; ArcController (Task 2).
#include <math.h>
#include <string.h>
#include <unity.h>

#include "core/arc_limits.h"
#include "core/motion_config.h"
#include "cq_protocol.h"

using namespace cq::core;
namespace proto = cq::proto;

void setUp() {}
void tearDown() {}

void test_arc_band_matches_spec_table() {
  const struct { uint16_t r; uint8_t lo, hi; } cases[] = {{31, 56, 56},  {40, 43, 64},  {49, 36, 69}, {60, 31, 74},
                                                           {80, 26, 80},  {100, 23, 83}, {150, 19, 88}};
  for (const auto& c : cases) {
    const ArcBand b = arcBand(c.r);
    TEST_ASSERT_TRUE(b.possible);
    TEST_ASSERT_EQUAL_UINT8(c.lo, b.lo);
    TEST_ASSERT_EQUAL_UINT8(c.hi, b.hi);
  }
}

void test_arc_too_tight_and_min_radius() {
  TEST_ASSERT_FALSE(arcBand(30).possible);  // inner needs > outer allows
  TEST_ASSERT_FALSE(arcBand(9).possible);   // inside B/2: the inner track would reverse
  TEST_ASSERT_FALSE(arcBand(0).possible);
  TEST_ASSERT_EQUAL_UINT16(31, arcMinRadiusCm());
  const ArcBand wide = arcBand(1000);
  TEST_ASSERT_TRUE(wide.possible);
  TEST_ASSERT_TRUE(wide.lo >= motion::kTurnWMin && wide.hi <= 100);
}

void test_arc_refusal_messages() {
  char buf[96];
  TEST_ASSERT_TRUE(arcRefusal(buf, sizeof(buf), 40) > 0);
  TEST_ASSERT_EQUAL_STRING("radius 40 cm: use speed 43-64 %, or pivot", buf);
  arcRefusal(buf, sizeof(buf), 31);
  TEST_ASSERT_EQUAL_STRING("radius 31 cm: use speed 56-56 %, or pivot", buf);
  arcRefusal(buf, sizeof(buf), 20);
  TEST_ASSERT_EQUAL_STRING("radius 20 cm too tight (min 31): use pivot", buf);
  TEST_ASSERT_EQUAL(0, arcRefusal(buf, 8, 40));  // does not fit -> 0
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_arc_band_matches_spec_table);
  RUN_TEST(test_arc_too_tight_and_min_radius);
  RUN_TEST(test_arc_refusal_messages);
  return UNITY_END();
}
