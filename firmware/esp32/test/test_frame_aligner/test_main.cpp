// FrameAligner + AckSwallow + FrameTx: transparent passthrough, whole chunks, injected STOP.
#include <string.h>
#include <unity.h>

#include "../support/frames.h"
#include "core/frame_aligner.h"
#include "core/frame_tx.h"

using cq::core::AckSwallow;
using cq::core::ChunkKind;
using cq::core::FrameAligner;
using cq::core::FrameTx;
namespace proto = cq::proto;

namespace {

struct Capture {
  uint8_t bytes[1024];
  size_t n = 0;
  ChunkKind kinds[64];
  size_t lens[64];
  size_t chunks = 0;
  void chunk(const uint8_t* d, size_t len, ChunkKind k) {
    memcpy(bytes + n, d, len);
    n += len;
    kinds[chunks] = k;
    lens[chunks++] = len;
  }
};

struct TxSink {  // what link_task does for the bench -> Uno direction
  FrameTx* tx;
  void chunk(const uint8_t* d, size_t len, ChunkKind) { tx->push(d, len); }
};

size_t heartbeat(uint8_t seq, uint8_t* out) { return cqtest::frame(proto::HeartbeatMsg{7}, seq, out); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_stream_is_forwarded_unchanged() {
  uint8_t in[128] = {0x01, 0x02};
  size_t n = 2;
  n += heartbeat(1, in + n);
  in[n++] = 0x55;
  n += heartbeat(2, in + n);
  FrameAligner a;
  Capture c;
  for (size_t i = 0; i < n; ++i) a.push(in + i, 1, 0, c);  // byte by byte
  TEST_ASSERT_EQUAL(n, c.n);
  TEST_ASSERT_EQUAL_MEMORY(in, c.bytes, n);
  TEST_ASSERT_EQUAL(2, a.frames());
  TEST_ASSERT_EQUAL(2, a.lastSeq());
}

void test_frames_are_single_chunks() {
  uint8_t in[64];
  const size_t n = heartbeat(9, in);
  FrameAligner a;
  Capture c;
  a.push(in, 3, 0, c);
  TEST_ASSERT_EQUAL(0, c.chunks);  // partial frame waits
  TEST_ASSERT_TRUE(a.midFrame());
  a.push(in + 3, n - 3, 1, c);
  TEST_ASSERT_EQUAL(1, c.chunks);
  TEST_ASSERT_EQUAL(n, c.lens[0]);
  TEST_ASSERT_EQUAL(static_cast<int>(ChunkKind::kFrame), static_cast<int>(c.kinds[0]));
}

void test_bad_crc_frame_forwarded() {
  uint8_t in[64];
  const size_t n = heartbeat(3, in);
  in[n - 1] ^= 0x5A;
  FrameAligner a;
  Capture c;
  a.push(in, n, 0, c);
  TEST_ASSERT_EQUAL(1, c.chunks);
  TEST_ASSERT_EQUAL(static_cast<int>(ChunkKind::kBadFrame), static_cast<int>(c.kinds[0]));
  TEST_ASSERT_EQUAL_MEMORY(in, c.bytes, n);
  TEST_ASSERT_EQUAL(1, a.badCrc());
}

void test_sync_with_bad_length_is_raw() {
  const uint8_t in[] = {0xAA, 0xC8, 0x10};
  FrameAligner a;
  Capture c;
  a.push(in, sizeof(in), 0, c);
  TEST_ASSERT_EQUAL(sizeof(in), c.n);
  TEST_ASSERT_EQUAL_MEMORY(in, c.bytes, sizeof(in));
  TEST_ASSERT_FALSE(a.midFrame());
}

void test_partial_flushed_raw_after_50ms() {
  uint8_t in[64];
  heartbeat(1, in);
  FrameAligner a;
  Capture c;
  a.push(in, 4, 100, c);
  a.flushIfStale(149, c);
  TEST_ASSERT_EQUAL(0, c.n);
  a.flushIfStale(150, c);
  TEST_ASSERT_EQUAL(4, c.n);
  TEST_ASSERT_EQUAL(static_cast<int>(ChunkKind::kRaw), static_cast<int>(c.kinds[0]));
  TEST_ASSERT_EQUAL(1, a.flushed());
}

void test_partial_discarded_on_close() {
  uint8_t in[64];
  heartbeat(1, in);
  FrameAligner a;
  Capture c;
  a.push(in, 5, 0, c);
  a.discardPartial();
  a.flushIfStale(1000, c);
  TEST_ASSERT_EQUAL(0, c.n);
}

void test_injected_frame_waits_for_partial() {
  FrameTx tx;
  TxSink sink{&tx};
  FrameAligner a;
  uint8_t bench[64];
  const size_t n = heartbeat(10, bench);
  a.push(bench, 4, 0, sink);  // bench frame half received
  const uint8_t stop[] = {0xAA, 0x01, 0x12, 0x8A, 0x01, 0x00};
  tx.pushUrgent(stop, sizeof(stop));
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(stop), tx.peek(p));  // nothing of the bench frame is in the queue yet
  tx.pop();
  a.push(bench + 4, n - 4, 1, sink);
  TEST_ASSERT_EQUAL(n, tx.peek(p));  // the bench frame follows whole
  TEST_ASSERT_EQUAL_MEMORY(bench, p, n);
}

void test_injected_stop_acks_swallowed() {
  AckSwallow s;
  uint8_t f[16];
  const uint8_t seq = static_cast<uint8_t>(200 + 128);  // lastSeq 200 + 128 wraps to 72
  const size_t n = cqtest::frame(proto::AckMsg{seq, 0}, 4, f);
  TEST_ASSERT_FALSE(s.shouldDrop(f, n, 0));  // not armed
  s.arm(seq, 500);
  TEST_ASSERT_TRUE(s.shouldDrop(f, n, 100));
  TEST_ASSERT_TRUE(s.shouldDrop(f, n, 150));  // resend's ACK too
  uint8_t other[16];
  const size_t m = cqtest::frame(proto::AckMsg{static_cast<uint8_t>(seq + 1), 0}, 5, other);
  TEST_ASSERT_FALSE(s.shouldDrop(other, m, 200));  // bench's own ACK passes
  TEST_ASSERT_FALSE(s.shouldDrop(f, n, 500));      // window over
  TEST_ASSERT_EQUAL(2, s.dropped());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_stream_is_forwarded_unchanged);
  RUN_TEST(test_frames_are_single_chunks);
  RUN_TEST(test_bad_crc_frame_forwarded);
  RUN_TEST(test_sync_with_bad_length_is_raw);
  RUN_TEST(test_partial_flushed_raw_after_50ms);
  RUN_TEST(test_partial_discarded_on_close);
  RUN_TEST(test_injected_frame_waits_for_partial);
  RUN_TEST(test_injected_stop_acks_swallowed);
  return UNITY_END();
}
