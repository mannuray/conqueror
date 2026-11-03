// Plan Z2a Task 4: SkillHub - one server, its things, the one lease, its deadman, stale messages (bridge LLD R26).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "core/skill_hub.h"

using namespace cq::core;
using cq::logo::AskQ;
using cq::logo::ThingNames;

void setUp() {}
void tearDown() {}

namespace {
constexpr int kA = 5, kB = 9;  // two sockets

// A list of n words "w0".."w{n-1}" with one name each ("n0"...) - storage is static (views).
ThingList makeList(int n, int names_each = 1) {
  static char word[ThingList::kMaxWords + 2][8], name[ThingList::kMaxWords + 2][8];
  ThingList l;
  l.n = static_cast<uint8_t>(n);
  for (int i = 0; i < n; ++i) {
    snprintf(word[i], sizeof word[i], "w%d", i);
    snprintf(name[i], sizeof name[i], "n%d", i);
    l.word[i] = word[i];
    l.n_names[i] = static_cast<uint8_t>(names_each);
    for (int j = 0; j < names_each && j < ThingList::kNameStride; ++j) l.names[i * ThingList::kNameStride + j] = name[i];
  }
  return l;
}

SkillHub hubWithServer(int fd = kA) {
  SkillHub h;
  TEST_ASSERT_EQUAL(static_cast<int>(SkillHub::Decision::kOk), static_cast<int>(h.onHello(fd, makeList(2))));
  return h;
}
int D(SkillHub::Decision d) { return static_cast<int>(d); }
}  // namespace

void test_one_server_only() {
  SkillHub h;
  TEST_ASSERT_EQUAL(-1, h.server());
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onHello(kA, makeList(1))));
  TEST_ASSERT_EQUAL(kA, h.server());
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kBusy), D(h.onHello(kB, makeList(1))));
  TEST_ASSERT_EQUAL(kA, h.server());
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onHello(kA, makeList(1))));  // the same one says hello again
  TEST_ASSERT_EQUAL(-1, h.onClose(kB));                                           // a stranger closing changes nothing
  TEST_ASSERT_EQUAL(kA, h.server());
  h.onClose(kA);
  TEST_ASSERT_EQUAL(-1, h.server());
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onHello(kB, makeList(1))));  // free again
}

void test_things_set_replaced_too_big_kept() {
  SkillHub h = hubWithServer();
  TEST_ASSERT_EQUAL_STRING("w1", h.things().resolve("n1"));
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onThings(kA, makeList(3))));
  TEST_ASSERT_EQUAL(3, h.things().size());
  TEST_ASSERT_EQUAL_STRING("w2", h.things().resolve("w2"));
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kTooBig), D(h.onThings(kA, makeList(48, 9))));  // 9 names
  TEST_ASSERT_EQUAL(3, h.things().size());                                                // old kept
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onThings(kA, makeList(48))));         // exactly 48 is fine
  TEST_ASSERT_EQUAL(48, h.things().size());
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kNotServer), D(h.onThings(kB, makeList(1))));
  TEST_ASSERT_EQUAL(48, h.things().size());
}

void test_a_too_big_hello_does_not_register() {
  SkillHub h;
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kTooBig), D(h.onHello(kA, makeList(48, 9))));
  TEST_ASSERT_EQUAL(-1, h.server());
}

void test_person_is_never_offered() {
  SkillHub h;
  ThingList l = makeList(2);
  l.word[1] = "person";
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onHello(kA, l)));
  TEST_ASSERT_NULL(h.things().resolve("person"));
  TEST_ASSERT_NOT_NULL(h.things().resolve("w0"));
}

void test_ask_answer_by_id() {
  SkillHub h = hubWithServer();
  const int32_t q = h.ask(AskQ::kSee, "w0", 1000);
  TEST_ASSERT_GREATER_THAN(0, q);
  TEST_ASSERT_TRUE(h.answerFor(kA, q));
  TEST_ASSERT_FALSE(h.answerFor(kA, q + 1));
  TEST_ASSERT_FALSE(h.answerFor(kB, q));  // not the server
  TEST_ASSERT_FALSE(h.answerFor(kA, -1));
  h.clearAsk();
  TEST_ASSERT_FALSE(h.answerFor(kA, q));  // an answer is used once
  SkillHub none;
  TEST_ASSERT_EQUAL(-1, none.ask(AskQ::kSee, "w0", 0));  // no server, no question
}

// Review Focus 2 (a): the answer to question N arrives while question N+1 is pending
void test_late_answer_dropped() {
  SkillHub h = hubWithServer();
  const int32_t n = h.ask(AskQ::kSee, "w0", 1000);
  const int32_t n1 = h.ask(AskQ::kHowfar, "w1", 1350);
  TEST_ASSERT_NOT_EQUAL(n, n1);
  TEST_ASSERT_FALSE(h.answerFor(kA, n));
  TEST_ASSERT_TRUE(h.answerFor(kA, n1));
  // and one that lapsed with no new question: gone after 300 ms
  SkillHub g = hubWithServer();
  const int32_t m = g.ask(AskQ::kSee, "w0", 1000);
  g.tick(1299);
  TEST_ASSERT_TRUE(g.answerFor(kA, m));
  g.tick(1301);
  TEST_ASSERT_FALSE(g.answerFor(kA, m));
}

// Review Focus 2 (b): a second end for skill N arrives while skill N+1 runs
void test_end_with_old_id_dropped() {
  SkillHub h = hubWithServer();
  const int32_t n = h.newId();
  TEST_ASSERT_TRUE(h.start(n, 1000));
  TEST_ASSERT_TRUE(h.endFor(kA, n));
  h.endLease();  // skill N done
  TEST_ASSERT_FALSE(h.endFor(kA, n));  // an end after the lease ended
  const int32_t n1 = h.newId();
  TEST_ASSERT_TRUE(h.start(n1, 2000));
  TEST_ASSERT_FALSE(h.endFor(kA, n));  // the repeated end of N while N+1 runs
  TEST_ASSERT_FALSE(h.leaseOk(kA, n, 2010));
  TEST_ASSERT_TRUE(h.endFor(kA, n1));
  TEST_ASSERT_FALSE(h.endFor(kB, n1));
}

void test_lease_only_for_holder_and_id() {
  SkillHub h = hubWithServer();
  TEST_ASSERT_FALSE(h.leaseOk(kA, 1, 0));  // no lease yet
  const int32_t id = h.newId();
  TEST_ASSERT_TRUE(h.start(id, 1000));
  TEST_ASSERT_TRUE(h.leaseOk(kA, id, 1050));
  TEST_ASSERT_FALSE(h.leaseOk(kA, id + 1, 1060));
  TEST_ASSERT_FALSE(h.leaseOk(kB, id, 1060));
  TEST_ASSERT_FALSE(h.leaseOk(kA, -1, 1060));
  h.endLease();
  TEST_ASSERT_FALSE(h.leaseOk(kA, id, 1070));
  SkillHub none;
  TEST_ASSERT_FALSE(none.start(1, 0));  // no server, no lease
}

void test_lease_deadman_300ms() {
  SkillHub h = hubWithServer();
  const int32_t id = h.newId();
  h.start(id, 1000);
  TEST_ASSERT_EQUAL(-1, h.tick(1300));  // exactly 300 ms: still alive
  TEST_ASSERT_TRUE(h.leaseOk(kA, id, 1290));  // a drive frame refreshes it
  TEST_ASSERT_EQUAL(-1, h.tick(1590));
  TEST_ASSERT_EQUAL(id, h.tick(1591));  // 301 ms of silence
  TEST_ASSERT_EQUAL(-1, h.tick(1700));  // reported once
  TEST_ASSERT_FALSE(h.leaseOk(kA, id, 1700));
  TEST_ASSERT_FALSE(h.endFor(kA, id));
  // a frame that arrives too late is refused even before tick ran
  const int32_t id2 = h.newId();
  h.start(id2, 5000);
  TEST_ASSERT_FALSE(h.leaseOk(kA, id2, 5301));
  TEST_ASSERT_EQUAL(id2, h.tick(5302));
}

void test_deadman_survives_the_clock_wrapping() {
  SkillHub h = hubWithServer();
  const int32_t id = h.newId();
  h.start(id, 0xFFFFFF00u);
  TEST_ASSERT_TRUE(h.leaseOk(kA, id, 0xFFFFFFF0u));
  TEST_ASSERT_EQUAL(-1, h.tick(100));  // 0x100 + 0x10 + ... = 116 ms after the last frame
  TEST_ASSERT_EQUAL(id, h.tick(400));
}

void test_close_ends_lease() {
  SkillHub h = hubWithServer();
  const int32_t id = h.newId();
  h.start(id, 1000);
  h.ask(AskQ::kSee, "w0", 1000);
  TEST_ASSERT_EQUAL(id, h.onClose(kA));
  TEST_ASSERT_EQUAL(-1, h.leaseId());
  TEST_ASSERT_EQUAL(-1, h.server());
  TEST_ASSERT_EQUAL(0, h.things().size());  // the AI helper is gone: no things
  TEST_ASSERT_EQUAL(-1, h.tick(2000));
  TEST_ASSERT_EQUAL(-1, h.onClose(kA));  // nothing left to end
  SkillHub g = hubWithServer();
  TEST_ASSERT_EQUAL(-1, g.onClose(kA));  // closed with no lease
}

// Review Focus 3: the engine dies mid-skill and comes back
void test_reconnect_does_not_get_old_lease() {
  SkillHub h = hubWithServer(kA);
  const int32_t id = h.newId();
  h.start(id, 1000);
  h.onClose(kA);
  // it reconnects - the same socket number is reused, the id is not
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onHello(kA, makeList(1))));
  TEST_ASSERT_FALSE(h.leaseOk(kA, id, 1100));
  TEST_ASSERT_FALSE(h.endFor(kA, id));
  const int32_t next = h.newId();
  TEST_ASSERT_NOT_EQUAL(id, next);
  // ... and a different socket after a close that was never delivered cannot take the lease either
  SkillHub g = hubWithServer(kA);
  const int32_t gid = g.newId();
  g.start(gid, 1000);
  g.onClose(kA);
  g.onHello(kB, makeList(1));
  TEST_ASSERT_FALSE(g.leaseOk(kB, gid, 1100));
}

// Review Focus 1: the worker copies the list when a check starts; a replacement afterwards cannot tear the copy.
void test_caps_copy_not_torn() {
  SkillHub h = hubWithServer();
  static ThingNames copy;
  copy = h.things();  // what the worker does under the lock, before the check runs
  TEST_ASSERT_EQUAL(D(SkillHub::Decision::kOk), D(h.onThings(kA, makeList(5))));  // skill_things arrives mid-check
  TEST_ASSERT_EQUAL(2, copy.size());
  TEST_ASSERT_EQUAL_STRING("w1", copy.resolve("n1"));
  TEST_ASSERT_NULL(copy.resolve("w4"));
  TEST_ASSERT_EQUAL(5, h.things().size());
  TEST_ASSERT_EQUAL_STRING("w4", h.things().resolve("w4"));
}

void test_ids_never_repeat_and_stay_positive() {
  SkillHub h = hubWithServer();
  int32_t last = 0;
  for (int i = 0; i < 1000; ++i) {
    const int32_t id = (i % 2) ? h.ask(AskQ::kSee, "w0", 0) : h.newId();
    TEST_ASSERT_GREATER_THAN(last, id);
    last = id;
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_one_server_only);
  RUN_TEST(test_things_set_replaced_too_big_kept);
  RUN_TEST(test_a_too_big_hello_does_not_register);
  RUN_TEST(test_person_is_never_offered);
  RUN_TEST(test_ask_answer_by_id);
  RUN_TEST(test_late_answer_dropped);
  RUN_TEST(test_end_with_old_id_dropped);
  RUN_TEST(test_lease_only_for_holder_and_id);
  RUN_TEST(test_lease_deadman_300ms);
  RUN_TEST(test_deadman_survives_the_clock_wrapping);
  RUN_TEST(test_close_ends_lease);
  RUN_TEST(test_reconnect_does_not_get_old_lease);
  RUN_TEST(test_caps_copy_not_torn);
  RUN_TEST(test_ids_never_repeat_and_stay_positive);
  return UNITY_END();
}
