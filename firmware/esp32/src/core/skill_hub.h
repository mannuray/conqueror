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

class SkillHub {
 public:
  enum class Decision : uint8_t { kOk, kBusy, kTooBig, kNotServer };
  static constexpr uint32_t kDeadmanMs = motion::kRateDeadmanMs;  // no skill_drive for 300 ms ends the lease
  static constexpr uint32_t kAskMs = 300;                          // a question waits this long

  // A hello with client "ai-skills": the one skill server. kBusy: another fd holds it. kTooBig: the list is over the
  // bounds - nothing registers and the old list stays.
  Decision onHello(int fd, const ThingList& t);
  // skill_things from the server: replaces the list (kTooBig keeps the old one). kNotServer from any other fd.
  Decision onThings(int fd, const ThingList& t);
  // The fd closed (or went silent). The id of the lease that ended with it, or -1. Clears the list and the question.
  int32_t onClose(int fd);
  int server() const { return m_server; }  // -1 = none
  const logo::ThingNames& things() const { return m_things; }

  // A question for the server: its new id, or -1 with no server. It replaces an earlier question, whose answer is
  // dropped from now on.
  int32_t ask(logo::AskQ q, const char* thing, uint32_t now);
  // True only for the pending question's id, from the server, inside its 300 ms (an answer is used once: clearAsk).
  bool answerFor(int fd, int32_t id) const;
  void clearAsk() { m_ask_id = -1; }

  // A fresh id for a skill; start() grants the lease to the server for it (replacing any older lease, whose id then
  // never matches). False with no server.
  int32_t newId();
  bool start(int32_t id, uint32_t now);
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
  bool holder(int fd, int32_t id) const { return m_lease_id >= 0 && id == m_lease_id && fd == m_lease_fd && fd == m_server; }
  int m_server = -1;
  logo::ThingNames m_things;
  int32_t m_next_id = 1;
  int32_t m_ask_id = -1;
  uint32_t m_ask_ms = 0;
  int32_t m_lease_id = -1;
  int m_lease_fd = -1;
  uint32_t m_last_drive_ms = 0;
};

}  // namespace core
}  // namespace cq
