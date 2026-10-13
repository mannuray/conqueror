// FrameParser: protocol/vectors/frames.json + the resync cases of Plan A's Python parser tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "../support/frames.h"
#include "core/frame_parser.h"

using cq::core::FrameParser;
using cq::core::FrameView;

namespace {

struct Seen {
  uint8_t types[64];
  size_t n = 0;
};

void collect(FrameParser& p, const uint8_t* d, size_t len, Seen& s, size_t chunk = 0) {
  if (chunk == 0) chunk = len;
  for (size_t i = 0; i < len; i += chunk) {
    const size_t n = (len - i < chunk) ? len - i : chunk;
    p.feed(d + i, n, [&s](const FrameView& f) {
      if (s.n < sizeof(s.types)) s.types[s.n++] = f.type;
    });
  }
}

size_t stopFrame(uint8_t* out) { return cqtest::frame(cq::proto::StopMsg{1}, 5, out); }  // golden 1
size_t turnFrame(uint8_t* out) {
  return cqtest::frame(cq::proto::TurnMsg{2, 0x81, -900, 40, 0}, 7, out);  // golden 2
}

// Reads every "hex": "aa .." string of protocol/vectors/frames.json.
size_t loadVectors(uint8_t* out, size_t cap, size_t& count, uint8_t* types) {
  FILE* f = fopen(CQ_VECTORS_PATH, "rb");
  if (!f) return 0;
  static char text[200000];
  const size_t len = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[len] = '\0';
  size_t n = 0;
  count = 0;
  for (const char* p = strstr(text, "\"hex\": \""); p; p = strstr(p, "\"hex\": \"")) {
    p += 8;
    types[count++] = static_cast<uint8_t>(strtoul(p + 6, nullptr, 16));  // "aa ll TT"
    while (*p != '"' && n < cap) {
      out[n++] = static_cast<uint8_t>(strtoul(p, const_cast<char**>(&p), 16));
      while (*p == ' ') ++p;
    }
  }
  return n;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_all_vectors_parse_in_order() {
  static uint8_t data[20000];
  uint8_t types[128];
  size_t count = 0;
  const size_t len = loadVectors(data, sizeof(data), count, types);
  TEST_ASSERT_GREATER_THAN(50, static_cast<int>(count));
  const size_t chunks[] = {0, 1, 7};
  for (size_t chunk : chunks) {
    FrameParser p;
    size_t got = 0;
    bool order_ok = true;
    for (size_t i = 0; i < len; i += (chunk ? chunk : len)) {
      const size_t n = chunk && len - i > chunk ? chunk : len - i;
      p.feed(data + i, n, [&](const FrameView& f) {
        if (got >= count || types[got] != f.type) order_ok = false;
        ++got;
      });
    }
    TEST_ASSERT_EQUAL(count, got);
    TEST_ASSERT_TRUE(order_ok);
    TEST_ASSERT_EQUAL(0, p.stats().bad_crc);
  }
}

void test_split_and_noise() {
  uint8_t d[32] = {0x00, 0x13, 0xFF};
  const size_t n = 3 + turnFrame(d + 3);
  FrameParser p;
  Seen s;
  collect(p, d, n, s, 1);
  TEST_ASSERT_EQUAL(1, s.n);
  TEST_ASSERT_EQUAL_HEX8(0x13, s.types[0]);
}

void test_bad_crc_then_good() {
  uint8_t d[32];
  const size_t g = stopFrame(d);
  memcpy(d + g, d, g);
  d[g - 1] ^= 0xFF;  // corrupt the first copy's CRC
  FrameParser p;
  Seen s;
  collect(p, d, 2 * g, s);
  TEST_ASSERT_EQUAL(1, p.stats().bad_crc);
  TEST_ASSERT_EQUAL(1, s.n);
  TEST_ASSERT_EQUAL_HEX8(0x12, s.types[0]);
}

void test_bad_len() {
  const uint8_t d[] = {0xAA, 49, 0x10, 0};
  FrameParser p;
  Seen s;
  collect(p, d, sizeof(d), s);
  TEST_ASSERT_EQUAL(0, s.n);
  TEST_ASSERT_EQUAL(1, p.stats().bad_len);
}

void test_resyncs_after_stray_sync() {
  uint8_t d[32] = {0xAA};
  const size_t n = 1 + stopFrame(d + 1);
  FrameParser p;
  Seen s;
  collect(p, d, n, s);
  TEST_ASSERT_EQUAL(1, s.n);
  TEST_ASSERT_EQUAL(1, p.stats().bad_len);
}

void test_recovers_good_frame_after_truncated_header() {
  const uint8_t junk1[] = {0xAA, 0x05};
  const uint8_t junk2[] = {0xAA, 0x03, 0x10, 0x00, 0x01};
  const uint8_t* junks[] = {junk1, junk2};
  const size_t jl[] = {sizeof(junk1), sizeof(junk2)};
  for (int j = 0; j < 2; ++j) {
    uint8_t d[64];
    memcpy(d, junks[j], jl[j]);
    size_t n = jl[j];
    for (int k = 0; k < 3; ++k) n += stopFrame(d + n);
    FrameParser p;
    Seen s;
    collect(p, d, n, s);
    TEST_ASSERT_EQUAL(3, s.n);
    TEST_ASSERT_EQUAL(1, p.stats().bad_crc);
  }
}

void test_noise_with_many_syncs_then_frames() {
  uint8_t d[96] = {0xAA, 0xAA, 0x00, 0xAA, 49, 0xAA, 0x02, 0xAA};
  size_t n = 8;
  n += stopFrame(d + n);
  n += turnFrame(d + n);
  n += stopFrame(d + n);
  const size_t chunks[] = {n, 1, 3};
  for (size_t chunk : chunks) {
    FrameParser p;
    Seen s;
    collect(p, d, n, s, chunk);
    TEST_ASSERT_EQUAL(3, s.n);
    TEST_ASSERT_EQUAL_HEX8(0x12, s.types[0]);
    TEST_ASSERT_EQUAL_HEX8(0x13, s.types[1]);
    TEST_ASSERT_EQUAL_HEX8(0x12, s.types[2]);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_all_vectors_parse_in_order);
  RUN_TEST(test_split_and_noise);
  RUN_TEST(test_bad_crc_then_good);
  RUN_TEST(test_bad_len);
  RUN_TEST(test_resyncs_after_stray_sync);
  RUN_TEST(test_recovers_good_frame_after_truncated_header);
  RUN_TEST(test_noise_with_many_syncs_then_frames);
  return UNITY_END();
}
