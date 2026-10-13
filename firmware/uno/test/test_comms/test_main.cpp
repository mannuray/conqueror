#include <string.h>
#include <unity.h>

#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"

using namespace cq;

void setUp() {}
void tearDown() {}

// Golden frames from protocol/defs/golden.yaml
static const uint8_t kStop[] = {0xAA, 0x01, 0x12, 0x05, 0x01, 0x24};
static const uint8_t kTurn[] = {0xAA, 0x07, 0x13, 0x07, 0x02, 0x81, 0x7C, 0xFC, 0x28, 0x00, 0x00, 0xE5};
static const uint8_t kAck[] = {0xAA, 0x02, 0x81, 0x03, 0x07, 0x00, 0x35};

// Feeds bytes the way commsPoll does (feed only when nothing is pending, then run next() with a
// step budget until the byte and its replay are done) and records frame types. A small budget
// splits the replay across many next() calls; results must not depend on it.
struct Got {
  uint8_t types[8];
  uint8_t n;
};
static uint8_t feedAll(FrameParser& p, const uint8_t* data, uint8_t len, Got& got,
                       uint8_t budget = 64) {
  uint8_t frames = 0;
  for (uint8_t i = 0; i < len; ++i) {
    if (!p.feed(data[i])) return 0xFF;  // refused only while pending(); the loop drains first
    while (p.pending()) {
      uint8_t left = budget;
      if (p.next(left)) {
        if (got.n < sizeof got.types) got.types[got.n++] = p.type();
        ++frames;
      }
    }
  }
  return frames;
}

void test_parser_decodes_golden_turn_with_noise() {
  FrameParser p;
  Got got{};
  const uint8_t noise[] = {0x00, 0x13, 0xFF};
  TEST_ASSERT_EQUAL(0, feedAll(p, noise, sizeof noise, got));
  TEST_ASSERT_EQUAL(0, feedAll(p, kTurn, sizeof kTurn - 1, got));
  TEST_ASSERT_TRUE(p.feed(kTurn[sizeof kTurn - 1]));
  uint8_t budget = 64;
  TEST_ASSERT_TRUE(p.next(budget));
  TEST_ASSERT_EQUAL(63, budget);  // one byte, one step: no replay
  TEST_ASSERT_FALSE(p.pending());
  TEST_ASSERT_EQUAL_HEX8(0x13, p.type());
  TEST_ASSERT_EQUAL(7, p.len());
  proto::TurnMsg m{};
  TEST_ASSERT_TRUE(proto::decode(p.payload(), p.len(), m));
  TEST_ASSERT_EQUAL(-900, m.angle);
  TEST_ASSERT_FALSE(p.next(budget));
}

void test_parser_bad_crc_then_good() {
  FrameParser p;
  uint8_t bad[sizeof kTurn];
  memcpy(bad, kTurn, sizeof kTurn);
  bad[sizeof bad - 1] ^= 0xFF;  // no 0xAA after the sync, so nothing to replay
  Got got{};
  TEST_ASSERT_EQUAL(0, feedAll(p, bad, sizeof bad, got));
  TEST_ASSERT_EQUAL(1, feedAll(p, kTurn, sizeof kTurn, got));
  TEST_ASSERT_EQUAL_HEX8(0x13, got.types[0]);
  TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
}

void test_parser_rejects_long_len() {
  FrameParser p;
  const uint8_t data[] = {0xAA, 49, 0x10, 0x00};
  Got got{};
  TEST_ASSERT_EQUAL(0, feedAll(p, data, sizeof data, got));
  TEST_ASSERT_EQUAL(1, p.errors().bad_len);
}

// Resync tests mirror protocol/generator/tests/test_python_runtime.py.
void test_parser_resyncs_after_stray_sync() {
  FrameParser p;
  Got got{};
  const uint8_t stray = proto::kSync;
  feedAll(p, &stray, 1, got);
  TEST_ASSERT_EQUAL(1, feedAll(p, kStop, sizeof kStop, got));  // its 0xAA is a bad len, then a sync
  TEST_ASSERT_EQUAL_HEX8(0x12, got.types[0]);
  TEST_ASSERT_EQUAL(1, p.errors().bad_len);
}

void test_parser_recovers_good_frame_after_truncated_header() {
  const uint8_t junk_a[] = {0xAA, 0x05};
  const uint8_t junk_b[] = {0xAA, 0x03, 0x10, 0x00, 0x01};
  const uint8_t* junks[] = {junk_a, junk_b};
  const uint8_t sizes[] = {sizeof junk_a, sizeof junk_b};
  for (uint8_t j = 0; j < 2; ++j) {
    FrameParser p;
    Got got{};
    // The false header waits for its claimed length, so the stream continues past it: the
    // trailing frames complete the false one, then the replay recovers every good frame.
    feedAll(p, junks[j], sizes[j], got);
    for (uint8_t k = 0; k < 3; ++k) feedAll(p, kStop, sizeof kStop, got);
    TEST_ASSERT_EQUAL(3, got.n);
    for (uint8_t k = 0; k < 3; ++k) TEST_ASSERT_EQUAL_HEX8(0x12, got.types[k]);
    TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
  }
}

void test_parser_noise_with_many_syncs_then_good_frames() {
  FrameParser p;
  Got got{};
  const uint8_t s = proto::kSync;
  const uint8_t noise[] = {s, s, 0x00, s, proto::kMaxPayload + 1, s, 0x02, s};
  feedAll(p, noise, sizeof noise, got);
  feedAll(p, kStop, sizeof kStop, got);
  feedAll(p, kTurn, sizeof kTurn, got);
  feedAll(p, kStop, sizeof kStop, got);
  TEST_ASSERT_EQUAL(3, got.n);
  TEST_ASSERT_EQUAL_HEX8(0x12, got.types[0]);
  TEST_ASSERT_EQUAL_HEX8(0x13, got.types[1]);
  TEST_ASSERT_EQUAL_HEX8(0x12, got.types[2]);
}

void test_parser_one_byte_can_release_two_frames() {
  FrameParser p;
  Got got{};
  // A false header claiming 9 payload bytes swallows exactly two STOP frames (3 + 9 = 12 bytes);
  // the CRC fails on the last byte and the replay completes both frames from that one byte.
  const uint8_t junk[] = {0xAA, 0x09};
  feedAll(p, junk, sizeof junk, got);
  feedAll(p, kStop, sizeof kStop, got);
  TEST_ASSERT_EQUAL(0, feedAll(p, kStop, sizeof kStop - 1, got));
  TEST_ASSERT_EQUAL(2, feedAll(p, &kStop[sizeof kStop - 1], 1, got));
  TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
}

// Replay stays pending across next() calls; feed() refuses a byte while it is pending and the
// replay survives. Same scenario as above, drained one step per call.
void test_parser_pending_replay_survives_across_calls() {
  FrameParser p;
  Got got{};
  const uint8_t junk[] = {0xAA, 0x09};
  feedAll(p, junk, sizeof junk, got);
  feedAll(p, kStop, sizeof kStop, got);
  feedAll(p, kStop, sizeof kStop - 1, got);
  TEST_ASSERT_TRUE(p.feed(kStop[sizeof kStop - 1]));
  uint8_t one = 1;
  TEST_ASSERT_FALSE(p.next(one));  // the CRC byte: mismatch, replay of 11 bytes queued
  TEST_ASSERT_EQUAL(0, one);
  TEST_ASSERT_TRUE(p.pending());
  TEST_ASSERT_FALSE(p.feed(0x00));  // refused: nothing pending is dropped or overwritten
  TEST_ASSERT_TRUE(p.pending());
  uint8_t frames = 0;
  uint8_t calls = 0;
  while (p.pending()) {
    uint8_t budget = 1;
    if (p.next(budget)) ++frames;
    ++calls;
  }
  TEST_ASSERT_EQUAL(2, frames);
  TEST_ASSERT_EQUAL(11, calls);  // 0xAA..0x24 twice, minus the leading sync already scanned
  TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
}

// Nested false headers (... 2e aa 2c aa 2a aa ...) maximise replay. Expected frames and
// counters come from conqueror_protocol.framing.FrameParser on the same bytes; they must not
// depend on how the step budget splits the work, and no next() call may exceed its budget.
static const uint8_t kAdversarial[] = {
    0x25, 0x58, 0x2a, 0x19, 0x1f, 0xaa, 0x07, 0x44, 0x07, 0xaa, 0xfd, 0xaa, 0x2f, 0x55, 0xaa,
    0x3d, 0x51, 0xaa, 0xad, 0x2a, 0x09, 0x1b, 0xe3, 0xaa, 0xaa, 0x13, 0x64, 0xa9, 0xaa, 0x6d,
    0x44, 0x82, 0xaa, 0xeb, 0xaa, 0xaa, 0x25, 0x21, 0x03, 0x25, 0xfb, 0xaa, 0x30, 0xaa, 0x2e,
    0xaa, 0x2c, 0xaa, 0x2a, 0xaa, 0x28, 0xaa, 0x26, 0xaa, 0x24, 0xaa, 0x22, 0xaa, 0x20, 0xaa,
    0x1e, 0xaa, 0x1c, 0xaa, 0x1a, 0xaa, 0x18, 0xaa, 0x16, 0xaa, 0x14, 0xaa, 0x12, 0xaa, 0x10,
    0xaa, 0x0e, 0xaa, 0x0c, 0xaa, 0x0a, 0xaa, 0x08, 0xaa, 0x06, 0xaa, 0x04, 0xaa, 0x02, 0xaa,
    0x00, 0xaa, 0x58, 0xaa, 0xaa, 0x60, 0x64, 0xaa, 0x1e, 0x00, 0x3e, 0xf5, 0x8a, 0x9d, 0xaa,
    0xe8, 0x29, 0x32, 0x0d, 0xe3, 0x1c, 0xaa, 0x1e, 0x11, 0x45, 0x1d, 0x0c, 0xaa, 0x38, 0x06};

void test_parser_adversarial_stream_matches_reference_under_any_budget() {
  const uint8_t budgets[] = {1, 7, 64, 255};
  for (uint8_t budget : budgets) {
    FrameParser p;
    Got got{};
    uint32_t max_one_byte = 0;  // steps spent on one received byte, replay included
    for (uint8_t i = 0; i < sizeof kAdversarial; ++i) {
      const uint32_t before = p.stepsTaken();
      feedAll(p, &kAdversarial[i], 1, got, budget);
      if (p.stepsTaken() - before > max_one_byte) max_one_byte = p.stepsTaken() - before;
    }
    // Good frames after the attack are all recovered, as in the reference.
    for (uint8_t k = 0; k < 3; ++k) {
      feedAll(p, kStop, sizeof kStop, got, budget);
      feedAll(p, kTurn, sizeof kTurn, got, budget);
    }
    feedAll(p, kStop, sizeof kStop, got, budget);
    TEST_ASSERT_EQUAL(7, got.n);
    for (uint8_t k = 0; k < 7; ++k) TEST_ASSERT_EQUAL_HEX8(k % 2 ? 0x13 : 0x12, got.types[k]);
    TEST_ASSERT_EQUAL(31, p.errors().bad_crc);
    TEST_ASSERT_EQUAL(12, p.errors().bad_len);
    TEST_ASSERT_LESS_OR_EQUAL(700, max_one_byte);  // the proven per-byte bound
    TEST_ASSERT_TRUE(max_one_byte > 255);          // so a budget really does split it
  }
}

void test_parser_next_never_exceeds_its_budget() {
  FrameParser p;
  for (uint8_t i = 0; i < sizeof kAdversarial; ++i) {
    TEST_ASSERT_TRUE(p.feed(kAdversarial[i]));
    while (p.pending()) {
      const uint8_t given = static_cast<uint8_t>(1 + (i % 16));
      uint8_t left = given;
      const uint32_t before = p.stepsTaken();
      p.next(left);
      const uint32_t used = p.stepsTaken() - before;
      TEST_ASSERT_LESS_OR_EQUAL(given, used);
      TEST_ASSERT_EQUAL(given - left, used);  // the budget accounts for every step taken
      TEST_ASSERT_TRUE(used > 0);             // progress while pending
    }
  }
}

void test_writer_matches_golden_ack() {
  FrameWriter w;
  w.setSeq(3);
  proto::AckMsg a{};
  a.ack_seq = 7;
  a.status = 0;
  uint8_t payload[proto::kMaxPayload];
  const uint8_t n = proto::encode(a, payload);
  uint8_t out[proto::kMaxPayload + 5];
  const uint8_t total = w.build(static_cast<uint8_t>(proto::FrameType::kAck), payload, n, out);
  TEST_ASSERT_EQUAL(sizeof kAck, total);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kAck, out, total);
  TEST_ASSERT_EQUAL(3, w.lastSeq());
}

void test_event_queue_overflow_counts_drops() {
  EventQueue q;
  for (int i = 0; i < 10; ++i) q.push(proto::EventKind::kBump, static_cast<int16_t>(i));
  TEST_ASSERT_EQUAL(2, q.dropped());
  Event e{};
  TEST_ASSERT_TRUE(q.peek(e));  // peek leaves it queued
  TEST_ASSERT_EQUAL(0, e.a);
  TEST_ASSERT_TRUE(q.pop(e));
  TEST_ASSERT_EQUAL(0, e.a);
  TEST_ASSERT_TRUE(q.pop(e));
  TEST_ASSERT_EQUAL(1, e.a);
}

void test_reject_limiter() {
  RejectLimiter r;
  TEST_ASSERT_TRUE(r.allow(1, 1000));
  TEST_ASSERT_FALSE(r.allow(1, 1200));
  TEST_ASSERT_TRUE(r.allow(2, 1200));
  TEST_ASSERT_TRUE(r.allow(1, 1500));
}

void test_link_session_alive_and_hello() {
  LinkSession s;
  TEST_ASSERT_FALSE(s.alive(0));
  TEST_ASSERT_TRUE(s.helloDue(0));      // first HELLO immediately
  TEST_ASSERT_FALSE(s.helloDue(100));
  TEST_ASSERT_TRUE(s.helloDue(600));    // every 500 ms until acked
  s.onValidFrame(600);
  s.onHelloAck();
  TEST_ASSERT_TRUE(s.synced());
  TEST_ASSERT_FALSE(s.helloDue(2000));
  TEST_ASSERT_TRUE(s.alive(850));
  TEST_ASSERT_FALSE(s.alive(901));      // 300 ms timeout
  s.onLinkLost();
  TEST_ASSERT_FALSE(s.synced());
  TEST_ASSERT_TRUE(s.helloDue(901));
}

// First contact after boot is not a restore (LINK_RESTORED only after a LINK_LOST).
void test_link_session_first_contact_is_not_a_restore() {
  LinkSession s;
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(0));
  s.onValidFrame(10);
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(10));    // first contact: no event
  s.onHelloAck();
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(300));
  TEST_ASSERT_EQUAL(LinkChange::kLost, s.poll(400));   // 300 ms silence
  TEST_ASSERT_FALSE(s.synced());                       // poll() ran onLinkLost()
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(410));
  s.onValidFrame(500);
  TEST_ASSERT_EQUAL(LinkChange::kRestored, s.poll(500));
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(510));
}

void test_link_session_request_resent_until_acked() {
  LinkSession s;
  uint8_t type = 0;
  s.setRequest(0x96);
  TEST_ASSERT_TRUE(s.requestDue(0, type));
  TEST_ASSERT_EQUAL_HEX8(0x96, type);
  s.onRequestSent(42, 0);
  TEST_ASSERT_FALSE(s.requestDue(100, type));
  TEST_ASSERT_TRUE(s.requestDue(500, type));
  s.onRequestSent(43, 500);
  s.onEspAck(42);  // stale ack
  TEST_ASSERT_TRUE(s.requestDue(1000, type));
  s.onEspAck(43);
  TEST_ASSERT_FALSE(s.requestDue(2000, type));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parser_decodes_golden_turn_with_noise);
  RUN_TEST(test_parser_bad_crc_then_good);
  RUN_TEST(test_parser_rejects_long_len);
  RUN_TEST(test_parser_resyncs_after_stray_sync);
  RUN_TEST(test_parser_recovers_good_frame_after_truncated_header);
  RUN_TEST(test_parser_noise_with_many_syncs_then_good_frames);
  RUN_TEST(test_parser_one_byte_can_release_two_frames);
  RUN_TEST(test_parser_pending_replay_survives_across_calls);
  RUN_TEST(test_parser_adversarial_stream_matches_reference_under_any_budget);
  RUN_TEST(test_parser_next_never_exceeds_its_budget);
  RUN_TEST(test_writer_matches_golden_ack);
  RUN_TEST(test_event_queue_overflow_counts_drops);
  RUN_TEST(test_reject_limiter);
  RUN_TEST(test_link_session_alive_and_hello);
  RUN_TEST(test_link_session_first_contact_is_not_a_restore);
  RUN_TEST(test_link_session_request_resent_until_acked);
  return UNITY_END();
}
