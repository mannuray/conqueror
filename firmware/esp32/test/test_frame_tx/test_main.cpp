// FrameTx: whole chunks only, urgent STOP at the next chunk boundary, bounded, no heap.
#include <string.h>
#include <unity.h>

#include "core/frame_tx.h"

using cq::core::FrameTx;

namespace {
FrameTx* g_tx = nullptr;
const uint8_t kA[] = {0xAA, 1, 2, 3};
const uint8_t kB[] = {0xAA, 4, 5};
const uint8_t kS[] = {0xAA, 9};
}  // namespace

void setUp() {
  static FrameTx tx;
  tx = FrameTx();
  g_tx = &tx;
}
void tearDown() {}

void test_fifo_whole_chunks() {
  TEST_ASSERT_TRUE(g_tx->push(kA, sizeof(kA)));
  TEST_ASSERT_TRUE(g_tx->push(kB, sizeof(kB)));
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));
  TEST_ASSERT_EQUAL_MEMORY(kA, p, sizeof(kA));
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kB), g_tx->peek(p));
  TEST_ASSERT_EQUAL_MEMORY(kB, p, sizeof(kB));
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));
}

void test_urgent_goes_first_when_idle() {
  g_tx->push(kA, sizeof(kA));
  g_tx->pushUrgent(kS, sizeof(kS));
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));
  TEST_ASSERT_EQUAL_MEMORY(kS, p, sizeof(kS));
}

void test_urgent_waits_for_chunk_boundary() {
  g_tx->push(kA, sizeof(kA));
  g_tx->push(kB, sizeof(kB));
  const uint8_t* p = nullptr;
  g_tx->peek(p);  // A is being written
  g_tx->pushUrgent(kS, sizeof(kS));
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));  // still A: never cut a chunk
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));  // STOP before B
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kB), g_tx->peek(p));
}

void test_full_queue_drops_and_counts() {
  for (size_t i = 0; i < FrameTx::kSlots; ++i) TEST_ASSERT_TRUE(g_tx->push(kA, sizeof(kA)));
  TEST_ASSERT_FALSE(g_tx->push(kA, sizeof(kA)));
  uint8_t big[FrameTx::kMaxChunk + 1] = {};
  TEST_ASSERT_FALSE(g_tx->pushUrgent(big, sizeof(big)));
  TEST_ASSERT_EQUAL(2, g_tx->dropped());
}

void test_clear_keeps_chunk_in_flight() {
  g_tx->push(kA, sizeof(kA));
  g_tx->push(kB, sizeof(kB));
  const uint8_t* p = nullptr;
  g_tx->peek(p);
  g_tx->clear();
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));
}

void test_stop_jumps_queue() {
  g_tx->push(kA, sizeof(kA), cq::core::ChunkClass::kMotion);  // DRIVE
  g_tx->push(kB, sizeof(kB));                                 // HEARTBEAT
  g_tx->push(kA, sizeof(kA), cq::core::ChunkClass::kMotion);  // CAM_JOY
  g_tx->pushUrgent(kS, sizeof(kS));
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));  // STOP first
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kB), g_tx->peek(p));  // control frames keep their order
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));  // no older drive restarts the tank after the STOP
  TEST_ASSERT_EQUAL(2, g_tx->purged());
}

void test_stop_never_cuts_motion_chunk_in_flight() {
  g_tx->push(kA, sizeof(kA), cq::core::ChunkClass::kMotion);
  g_tx->push(kA, sizeof(kA), cq::core::ChunkClass::kMotion);
  const uint8_t* p = nullptr;
  g_tx->peek(p);  // the first drive is being written
  g_tx->pushUrgent(kS, sizeof(kS));
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));  // finishes whole (frame-aligned)
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));
  TEST_ASSERT_EQUAL(1, g_tx->purged());
}

void test_clear_keeps_urgent_stop() {
  g_tx->push(kA, sizeof(kA));
  g_tx->pushUrgent(kS, sizeof(kS));
  g_tx->clear();  // master change drops queued ESP32 frames, never a pending STOP
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fifo_whole_chunks);
  RUN_TEST(test_urgent_goes_first_when_idle);
  RUN_TEST(test_urgent_waits_for_chunk_boundary);
  RUN_TEST(test_full_queue_drops_and_counts);
  RUN_TEST(test_clear_keeps_chunk_in_flight);
  RUN_TEST(test_stop_jumps_queue);
  RUN_TEST(test_stop_never_cuts_motion_chunk_in_flight);
  RUN_TEST(test_clear_keeps_urgent_stop);
  return UNITY_END();
}
