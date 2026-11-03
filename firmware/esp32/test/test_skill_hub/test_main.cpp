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

// A lock that records what happens and can refuse the take.
struct FakeLock {
  bool allow = true;
  int takes = 0, gives = 0;
  uint32_t last_wait = 0;
  char log[32] = "";
  static bool take(void* c, uint32_t wait) {
    FakeLock* f = static_cast<FakeLock*>(c);
    ++f->takes;
    f->last_wait = wait;
    if (f->allow) strcat(f->log, "T");
    return f->allow;
  }
  static void give(void* c) {
    FakeLock* f = static_cast<FakeLock*>(c);
    ++f->gives;
    strcat(f->log, "G");
  }
  LockPort port() { return LockPort{take, give, this}; }
};

int D(SkillHub::Decision d) { return static_cast<int>(d); }
using Dec = SkillHub::Decision;

// Stage a whole list in one part and commit it (the lock is free).
void put(SkillHub& h, int fd, const ThingList& l) {
  TEST_ASSERT_EQUAL(D(Dec::kReady), D(h.onThingsPart(fd, 1, 1, l)));
  FakeLock lock;
  LockPort port = lock.port();
  TEST_ASSERT_TRUE(h.tryCommit(port));
}

SkillHub hubWithServer(int fd = kA) {
  SkillHub h;
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onHello(fd)));
  put(h, fd, makeList(2));
  return h;
}
}  // namespace

void test_one_server_only() {
  SkillHub h;
  TEST_ASSERT_EQUAL(-1, h.server());
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onHello(kA)));
  TEST_ASSERT_EQUAL(kA, h.server());
  TEST_ASSERT_EQUAL(D(Dec::kBusy), D(h.onHello(kB)));
  TEST_ASSERT_EQUAL(kA, h.server());
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onHello(kA)));  // the same one says hello again
  TEST_ASSERT_EQUAL(-1, h.onClose(kB));              // a stranger closing changes nothing
  TEST_ASSERT_EQUAL(kA, h.server());
  h.onClose(kA);
  TEST_ASSERT_EQUAL(-1, h.server());
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onHello(kB)));  // free again
}

void test_hello_registers_without_things() {
  SkillHub h;
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onHello(kA)));
  TEST_ASSERT_EQUAL(kA, h.server());
  TEST_ASSERT_EQUAL(0, h.things().size());  // empty until the first commit
  TEST_ASSERT_FALSE(h.commitPending());
}

void test_things_in_parts_commit_on_the_last() {
  SkillHub h;
  h.onHello(kA);
  FakeLock lock;
  LockPort port = lock.port();
  ThingList p1 = makeList(2), p2;
  p2.n = 1;
  p2.word[0] = "w0";  // a word again in a later part: more names
  static const char* extra = "extra";
  p2.n_names[0] = 1;
  p2.names[0] = extra;
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 2, p1)));
  TEST_ASSERT_FALSE(h.commitPending());
  TEST_ASSERT_EQUAL(0, h.things().size());  // nothing visible before the last part
  TEST_ASSERT_EQUAL(D(Dec::kReady), D(h.onThingsPart(kA, 2, 2, p2)));
  TEST_ASSERT_TRUE(h.commitPending());
  TEST_ASSERT_EQUAL(0, h.things().size());  // ... nor until the lock commits it
  TEST_ASSERT_TRUE(h.tryCommit(port));
  TEST_ASSERT_FALSE(h.commitPending());
  TEST_ASSERT_EQUAL(2, h.things().size());
  TEST_ASSERT_EQUAL_STRING("w0", h.things().resolve("extra"));
  TEST_ASSERT_EQUAL_STRING("w0", h.things().resolve("n0"));
  TEST_ASSERT_EQUAL_STRING("TG", lock.log);  // taken, flipped, given - once
  TEST_ASSERT_FALSE(h.tryCommit(port));       // nothing more to commit: the lock is not touched
  TEST_ASSERT_EQUAL(1, lock.takes);
}

void test_a_new_list_replaces_the_committed_one() {
  SkillHub h = hubWithServer();
  put(h, kA, makeList(3));
  TEST_ASSERT_EQUAL(3, h.things().size());
  TEST_ASSERT_EQUAL_STRING("w2", h.things().resolve("w2"));
  put(h, kA, makeList(1));
  TEST_ASSERT_EQUAL(1, h.things().size());
  TEST_ASSERT_NULL(h.things().resolve("w2"));
}

void test_part_errors_drop_the_staging_and_keep_the_list() {
  SkillHub h = hubWithServer();  // committed: w0, w1
  ThingList l = makeList(3);
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 2, 2, l)));  // starts at 2
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 3, l)));
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 3, 3, l)));  // 2 skipped
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 2, 3, l)));  // staging was dropped: not part 1 any more
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 3, l)));
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 2, 4, l)));  // of changed
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 3, l)));
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 1, 0, l)));  // part > of
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 2, l)));
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 2, l)));  // part 1 again restarts
  TEST_ASSERT_EQUAL(D(Dec::kReady), D(h.onThingsPart(kA, 2, 2, makeList(1))));
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 2, 2, l)));  // a repeat of the last part
  TEST_ASSERT_FALSE(h.commitPending());  // the half-done list never reaches the checker
  TEST_ASSERT_EQUAL(2, h.things().size());
  TEST_ASSERT_EQUAL_STRING("w1", h.things().resolve("n1"));
}

void test_over_bounds_across_parts_is_too_big() {
  SkillHub h = hubWithServer();
  ThingList a = makeList(30), b = makeList(30);
  static char w[30][8];
  for (int i = 0; i < 30; ++i) {  // 30 more, different words: 60 in all
    snprintf(w[i], sizeof w[i], "x%d", i);
    b.word[i] = w[i];
  }
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 2, a)));
  TEST_ASSERT_EQUAL(D(Dec::kTooBig), D(h.onThingsPart(kA, 2, 2, b)));
  TEST_ASSERT_FALSE(h.commitPending());
  TEST_ASSERT_EQUAL(2, h.things().size());  // the committed list is kept
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kA, 2, 2, makeList(1))));  // and the staging is gone
  // too many names for one word over two parts
  ThingList n1 = makeList(1, 5), n2 = makeList(1, 5);
  static const char* more[5] = {"m1", "m2", "m3", "m4", "m5"};
  for (int j = 0; j < 5; ++j) n2.names[j] = more[j];
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onThingsPart(kA, 1, 2, n1)));
  TEST_ASSERT_EQUAL(D(Dec::kTooBig), D(h.onThingsPart(kA, 2, 2, n2)));
  TEST_ASSERT_EQUAL(2, h.things().size());
}

void test_parts_only_from_the_server() {
  SkillHub h = hubWithServer();
  TEST_ASSERT_EQUAL(D(Dec::kNotServer), D(h.onThingsPart(kB, 1, 1, makeList(1))));
  SkillHub none;
  TEST_ASSERT_EQUAL(D(Dec::kNotServer), D(none.onThingsPart(kA, 1, 1, makeList(1))));
}

void test_person_is_never_offered() {
  SkillHub h;
  h.onHello(kA);
  ThingList l = makeList(2);
  l.word[1] = "person";
  put(h, kA, l);
  TEST_ASSERT_NULL(h.things().resolve("person"));
  TEST_ASSERT_NOT_NULL(h.things().resolve("w0"));
}

// Minor 2: the link task never waits long for the lock; a refused take is retried on the next loop.
void test_commit_retried_when_the_lock_is_busy() {
  SkillHub h = hubWithServer();  // w0, w1 committed
  FakeLock lock;
  LockPort port = lock.port();
  h.onThingsPart(kA, 1, 1, makeList(4));
  lock.allow = false;
  TEST_ASSERT_FALSE(h.tryCommit(port));
  TEST_ASSERT_TRUE(h.commitPending());
  TEST_ASSERT_EQUAL(2, h.things().size());  // the old list, whole
  TEST_ASSERT_EQUAL(0, lock.gives);          // a take that failed is not given back
  TEST_ASSERT_TRUE(lock.last_wait <= SkillHub::kCommitWaitMs);  // bounded
  TEST_ASSERT_TRUE(lock.last_wait > 0 && lock.last_wait < 50);
  TEST_ASSERT_FALSE(h.tryCommit(port));
  lock.allow = true;
  TEST_ASSERT_TRUE(h.tryCommit(port));  // the next loop
  TEST_ASSERT_EQUAL(4, h.things().size());
  TEST_ASSERT_FALSE(h.commitPending());
}

// Minor 3 / Review Focus 1: the checker's copy is taken inside the lock: take -> copy -> give.
struct CopyProbe {
  SkillHub* hub;
  cq::logo::ThingNames* dst;
  bool copied_at_take = true, copied_at_give = false;
  FakeLock inner;
};
bool probeTake(void* c, uint32_t wait) {
  CopyProbe* p = static_cast<CopyProbe*>(c);
  p->copied_at_take = p->dst->size() != 0;
  return FakeLock::take(&p->inner, wait);
}
void probeGive(void* c) {
  CopyProbe* p = static_cast<CopyProbe*>(c);
  p->copied_at_give = p->dst->size() != 0;
  FakeLock::give(&p->inner);
}

void test_copy_for_check_copies_inside_the_lock() {
  SkillHub h = hubWithServer();
  static cq::logo::ThingNames copy;
  copy.clear();
  CopyProbe probe;
  probe.hub = &h;
  probe.dst = &copy;
  const LockPort port{probeTake, probeGive, &probe};
  TEST_ASSERT_TRUE(copyForCheck(h, copy, port));  // the helper caps() uses: ai = a server is there
  TEST_ASSERT_FALSE(probe.copied_at_take);
  TEST_ASSERT_TRUE(probe.copied_at_give);
  TEST_ASSERT_EQUAL_STRING("TG", probe.inner.log);
  TEST_ASSERT_EQUAL(LockPort::kForever, probe.inner.last_wait);  // the worker may wait
  // a list committed after the copy never changes the copy
  put(h, kA, makeList(5));
  TEST_ASSERT_EQUAL(2, copy.size());
  TEST_ASSERT_NULL(copy.resolve("w4"));
  // no server: no copy, ai false (the lock is still taken for the read)
  SkillHub none;
  TEST_ASSERT_FALSE(copyForCheck(none, copy, port));
}

void test_ask_answer_by_id() {
  SkillHub h = hubWithServer();
  const int32_t q = h.ask(AskQ::kSee, "w0", 1000);
  TEST_ASSERT_GREATER_THAN(0, q);
  TEST_ASSERT_TRUE(h.answerFor(kA, q, 1100));
  TEST_ASSERT_FALSE(h.answerFor(kA, q + 1, 1100));
  TEST_ASSERT_FALSE(h.answerFor(kB, q, 1100));  // not the server
  TEST_ASSERT_FALSE(h.answerFor(kA, -1, 1100));
  h.clearAsk();
  TEST_ASSERT_FALSE(h.answerFor(kA, q, 1100));  // an answer is used once
  SkillHub none;
  TEST_ASSERT_EQUAL(-1, none.ask(AskQ::kSee, "w0", 0));  // no server, no question
}

// Review Focus 2 (a): the answer to question N arrives while question N+1 is pending
void test_late_answer_dropped() {
  SkillHub h = hubWithServer();
  const int32_t n = h.ask(AskQ::kSee, "w0", 1000);
  const int32_t n1 = h.ask(AskQ::kHowfar, "w1", 1350);
  TEST_ASSERT_NOT_EQUAL(n, n1);
  TEST_ASSERT_FALSE(h.answerFor(kA, n, 1360));
  TEST_ASSERT_TRUE(h.answerFor(kA, n1, 1360));
}

// Minor 1: an answer after the question's 300 ms is refused even if tick() has not run yet
void test_answer_after_the_deadline_dropped() {
  SkillHub g = hubWithServer();
  const int32_t m = g.ask(AskQ::kSee, "w0", 1000);
  TEST_ASSERT_TRUE(g.answerFor(kA, m, 1300));
  TEST_ASSERT_FALSE(g.answerFor(kA, m, 1301));
  const int32_t m2 = g.ask(AskQ::kSee, "w0", 0xFFFFFF00u);  // across the clock wrap
  TEST_ASSERT_TRUE(g.answerFor(kA, m2, 0x20));
  TEST_ASSERT_FALSE(g.answerFor(kA, m2, 0x200));
  const int32_t m3 = g.ask(AskQ::kSee, "w0", 5000);
  g.tick(5301);  // and the tick lets it lapse for good
  TEST_ASSERT_FALSE(g.answerFor(kA, m3, 5301));
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

// Minor 4: starting a skill while a lease is held says which one it replaced
void test_start_reports_the_replaced_lease() {
  SkillHub h = hubWithServer();
  int32_t replaced = 99;
  const int32_t a = h.newId();
  TEST_ASSERT_TRUE(h.start(a, 1000, &replaced));
  TEST_ASSERT_EQUAL(-1, replaced);
  const int32_t b = h.newId();
  TEST_ASSERT_TRUE(h.start(b, 1100, &replaced));
  TEST_ASSERT_EQUAL(a, replaced);
  TEST_ASSERT_FALSE(h.leaseOk(kA, a, 1110));
  TEST_ASSERT_TRUE(h.leaseOk(kA, b, 1110));
  h.endLease();
  TEST_ASSERT_TRUE(h.start(h.newId(), 1200, &replaced));
  TEST_ASSERT_EQUAL(-1, replaced);
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
  FakeLock lock;
  LockPort port = lock.port();
  TEST_ASSERT_TRUE(h.commitPending());  // the AI helper is gone: its things go with the next commit
  TEST_ASSERT_TRUE(h.tryCommit(port));
  TEST_ASSERT_EQUAL(0, h.things().size());
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
  TEST_ASSERT_EQUAL(D(Dec::kOk), D(h.onHello(kA)));
  TEST_ASSERT_FALSE(h.leaseOk(kA, id, 1100));
  TEST_ASSERT_FALSE(h.endFor(kA, id));
  const int32_t next = h.newId();
  TEST_ASSERT_NOT_EQUAL(id, next);
  // ... and a different socket after a close that was never delivered cannot take the lease either
  SkillHub g = hubWithServer(kA);
  const int32_t gid = g.newId();
  g.start(gid, 1000);
  g.onClose(kA);
  g.onHello(kB);
  TEST_ASSERT_FALSE(g.leaseOk(kB, gid, 1100));
}

void test_close_drops_the_staging_and_a_new_server_starts_empty() {
  SkillHub h = hubWithServer();  // w0, w1 committed
  h.onThingsPart(kA, 1, 2, makeList(3));
  h.onClose(kA);
  h.onHello(kB);
  TEST_ASSERT_EQUAL(D(Dec::kBadPart), D(h.onThingsPart(kB, 2, 2, makeList(1))));  // the old staging is gone
  FakeLock lock;
  LockPort port = lock.port();
  TEST_ASSERT_TRUE(h.tryCommit(port));
  TEST_ASSERT_EQUAL(0, h.things().size());  // the old server's things are not the new one's
  put(h, kB, makeList(1));
  TEST_ASSERT_EQUAL(1, h.things().size());
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
  RUN_TEST(test_hello_registers_without_things);
  RUN_TEST(test_things_in_parts_commit_on_the_last);
  RUN_TEST(test_a_new_list_replaces_the_committed_one);
  RUN_TEST(test_part_errors_drop_the_staging_and_keep_the_list);
  RUN_TEST(test_over_bounds_across_parts_is_too_big);
  RUN_TEST(test_parts_only_from_the_server);
  RUN_TEST(test_person_is_never_offered);
  RUN_TEST(test_commit_retried_when_the_lock_is_busy);
  RUN_TEST(test_ask_answer_by_id);
  RUN_TEST(test_late_answer_dropped);
  RUN_TEST(test_answer_after_the_deadline_dropped);
  RUN_TEST(test_start_reports_the_replaced_lease);
  RUN_TEST(test_end_with_old_id_dropped);
  RUN_TEST(test_lease_only_for_holder_and_id);
  RUN_TEST(test_lease_deadman_300ms);
  RUN_TEST(test_deadman_survives_the_clock_wrapping);
  RUN_TEST(test_close_ends_lease);
  RUN_TEST(test_reconnect_does_not_get_old_lease);
  RUN_TEST(test_close_drops_the_staging_and_a_new_server_starts_empty);
  RUN_TEST(test_copy_for_check_copies_inside_the_lock);
  RUN_TEST(test_ids_never_repeat_and_stay_positive);
  return UNITY_END();
}
