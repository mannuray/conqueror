// The skill server's registration, its things list, the one drive lease and its deadman (bridge LLD R26.1-R26.3).
// Pure and host-tested; link task only. Every id (a question's or a skill's) comes from one counter that never runs
// back, so an old answer / end / drive frame - from before a STOP, a deadman or a reconnect - can never match a
// newer question or lease: the RunController's onAskAnswer / onSkillEnd take no id, this is where stale ones die.
#pragma once

#include <stdint.h>

#include "core/motion_config.h"
#include "core/thing_list.h"
#include "logo/program.h"
#include "logo/thing_names.h"

namespace cq {
namespace core {

// A lock as function pointers (no virtual): the SkillHub's things list is read by the Logo worker's checks and
// changed on the link task. take(ctx, wait_ms) false = not taken (nothing to give back); kForever waits.
struct LockPort {
  static constexpr uint32_t kForever = 0xFFFFFFFFu;
  bool (*take)(void* ctx, uint32_t wait_ms);
  void (*give)(void* ctx);
  void* ctx;
};

class SkillHub {
 public:
  // kOk: part staged, more to come. kReady: the last part is staged - tryCommit() makes the list the checkers' list.
  // kBadPart (-> nack BAD_FIELD): out of order, `of` changed, part > of. kTooBig (-> nack TOO_BIG): over 48 words /
  // 8 names / 24 bytes over everything staged so far. Both drop the staging; the committed list is kept.
  enum class Decision : uint8_t { kOk, kReady, kBusy, kBadPart, kTooBig, kNotServer };
  static constexpr uint32_t kDeadmanMs = motion::kRateDeadmanMs;  // no skill_drive for 300 ms ends the lease
  static constexpr uint32_t kAskMs = 300;                          // a question waits this long
  static constexpr uint32_t kCommitWaitMs = 2;                     // the link task never waits longer for the lock

  // A hello with client "ai-skills": the one skill server (things follow in skill_things parts). kBusy: another fd.
  Decision onHello(int fd);
  // One `skill_things` frame (part of `of`, 1-based). Only the link task touches the staging: no lock needed here.
  Decision onThingsPart(int fd, uint8_t part, uint8_t of, const ThingList& t);
  bool commitPending() const { return m_commit || m_clear; }
  bool clearPending() const { return m_clear; }  // the live list is the last server's, to be emptied by tryCommit
  // Makes a finished staging (or the emptiness after a close) the live list: takes the lock for at most
  // kCommitWaitMs and only for the swap. False: nothing to do, or the lock was busy - call again next loop.
  bool tryCommit(const LockPort& lock);
  // The fd closed (or went silent). The id of the lease that ended with it, or -1. Drops the staging; the live list
  // empties with the next tryCommit().
  int32_t onClose(int fd);
  int server() const { return m_server; }  // -1 = none
  const logo::ThingNames& things() const { return m_sets[m_live]; }  // the committed list; read under the lock

  // A question for the server: its new id, or -1 with no server. It replaces an earlier question, whose answer is
  // dropped from now on.
  int32_t ask(logo::AskQ q, const char* thing, uint32_t now);
  // True only for the pending question's id, from the server, inside its 300 ms (an answer is used once: clearAsk).
  bool answerFor(int fd, int32_t id, uint32_t now) const;
  void clearAsk() { m_ask_id = -1; }

  // A fresh id for a skill; start() grants the lease to the server for it (replacing any older lease, whose id then
  // never matches); `replaced` gets that lease's id (or -1) so the caller can send it a skill_end. False: no server.
  int32_t newId();
  bool start(int32_t id, uint32_t now, int32_t* replaced = nullptr);
  // The server's skill_done / skill_failed: true only for the running lease's id from the server (it is not ended
  // here - the controller stops the tank and calls endLease()).
  bool endFor(int fd, int32_t id) const;
  // A skill_drive: true only from the holder, for the lease's id, with no 300 ms gap; also keeps the lease alive.
  bool leaseOk(int fd, int32_t id, uint32_t now);
  // The deadman: the lease id that ran out (the lease ends), else -1. Also lets an unanswered question lapse.
  int32_t tick(uint32_t now);
  void endLease() { m_lease_id = -1; }
  int32_t leaseId() const { return m_lease_id; }  // -1 = none

 private:
  bool stage(const ThingList& t);
  Decision dropStaging(Decision why);
  void beginStaging(uint8_t of);
  bool partInOrder(uint8_t part, uint8_t of) const;
  bool holder(int fd, int32_t id) const { return m_lease_id >= 0 && id == m_lease_id && fd == m_server; }
  int m_server = -1;
  logo::ThingNames m_sets[2];  // the live list and the one being staged (a commit flips m_live)
  uint8_t m_live = 0;
  uint8_t m_next_part = 0, m_of = 0;  // staging: the part expected next (0 = none staging)
  bool m_commit = false, m_clear = false;
  int32_t m_next_id = 1;
  int32_t m_ask_id = -1;
  uint32_t m_ask_ms = 0;
  int32_t m_lease_id = -1;
  uint32_t m_last_drive_ms = 0;
};

// What the Logo worker's caps() does when a check starts: under the lock, `dst` becomes a copy of the live list (only
// the used entries) when a server is there. Returns that (Caps::ai). Waits for the lock: it is the worker's.
bool copyForCheck(const SkillHub& hub, logo::ThingNames& dst, const LockPort& lock);

}  // namespace core
}  // namespace cq
