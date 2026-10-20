// The MJPEG part header with the frame's capture time (Plan Q1, AI engine LLD §2).
#include <string.h>
#include <unity.h>

#include "core/stream_header.h"

using cq::core::mjpegPartHeader;
using cq::core::u64Str;

void setUp() {}
void tearDown() {}

void test_u64_prints_beyond_32_bits() {
  char b[24];
  TEST_ASSERT_EQUAL(1, u64Str(b, sizeof(b), 0));
  TEST_ASSERT_EQUAL_STRING("0", b);
  TEST_ASSERT_EQUAL(20, u64Str(b, sizeof(b), 18446744073709551615ULL));
  TEST_ASSERT_EQUAL_STRING("18446744073709551615", b);
  TEST_ASSERT_EQUAL(13, u64Str(b, sizeof(b), 4294967296123ULL));
  TEST_ASSERT_EQUAL_STRING("4294967296123", b);
}

void test_u64_refuses_a_small_buffer() {
  char b[4];
  TEST_ASSERT_EQUAL(0, u64Str(b, sizeof(b), 12345));
  TEST_ASSERT_EQUAL_STRING("", b);
}

void test_part_header_carries_capture_time_and_seq() {
  char b[160];
  const size_t n = mjpegPartHeader(b, sizeof(b), "cqframe", 5120, 123456789012ULL, 7);
  TEST_ASSERT_EQUAL_STRING(
      "\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: 5120\r\n"
      "X-Frame-Us: 123456789012\r\nX-Frame-Seq: 7\r\n\r\n",
      b);
  TEST_ASSERT_EQUAL(strlen(b), n);
}

void test_part_header_too_small_buffer_returns_zero() {
  char b[40];
  TEST_ASSERT_EQUAL(0, mjpegPartHeader(b, sizeof(b), "cqframe", 5120, 1, 1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_u64_prints_beyond_32_bits);
  RUN_TEST(test_u64_refuses_a_small_buffer);
  RUN_TEST(test_part_header_carries_capture_time_and_seq);
  RUN_TEST(test_part_header_too_small_buffer_returns_zero);
  return UNITY_END();
}
