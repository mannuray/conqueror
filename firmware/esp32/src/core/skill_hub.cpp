#include "core/skill_hub.h"

namespace cq {
namespace core {

SkillHub::Decision SkillHub::onHello(int fd) {
  if (m_server >= 0 && m_server != fd) return Decision::kBusy;
  m_server = fd;
  return Decision::kOk;
}

// Adds a part's words to the staging; false = over a bound.
bool SkillHub::stage(const ThingList& t) {
  logo::ThingNames& st = m_sets[1 - m_live];
  for (uint8_t w = 0; w < t.n; ++w) {
    if (!st.add(t.word[w], t.names + w * ThingList::kNameStride, t.n_names[w])) return false;
  }
  return true;
}

// A new list: whatever was half-staged is dropped, and a finished one not yet committed too.
void SkillHub::beginStaging(uint8_t of) {
  m_sets[1 - m_live].clear();
  m_next_part = 1;
  m_of = of;
  m_commit = false;
}

bool SkillHub::partInOrder(uint8_t part, uint8_t of) const {
  return of >= 1 && of <= ThingList::kMaxWords && part <= of && part == m_next_part && of == m_of;
}

// An error: the staging (finished or not) is dropped; the committed list stays.
SkillHub::Decision SkillHub::dropStaging(Decision why) {
  m_next_part = 0;
  m_commit = false;
  return why;
}

SkillHub::Decision SkillHub::onThingsPart(int fd, uint8_t part, uint8_t of, const ThingList& t) {
  if (fd != m_server || m_server < 0) return Decision::kNotServer;
  if (part == 1) beginStaging(of);
  if (!partInOrder(part, of)) return dropStaging(Decision::kBadPart);
  if (!stage(t)) return dropStaging(Decision::kTooBig);
  if (part < of) {
    ++m_next_part;
    return Decision::kOk;
  }
  m_next_part = 0;
  m_commit = true;
  return Decision::kReady;
}

bool SkillHub::tryCommit(const LockPort& lock) {
  if (!commitPending() || !lock.take(lock.ctx, kCommitWaitMs)) return false;
  if (m_clear) m_sets[m_live].clear();
  if (m_commit) m_live = static_cast<uint8_t>(1 - m_live);
  m_commit = false;
  m_clear = false;
  lock.give(lock.ctx);
  return true;
}

int32_t SkillHub::onClose(int fd) {
  if (fd < 0 || fd != m_server) return -1;
  const int32_t lease = m_lease_id;
  m_server = -1;
  m_sets[1 - m_live].clear();
  m_next_part = 0;
  m_commit = false;
  m_clear = true;
  m_ask_id = -1;
  m_lease_id = -1;
  return lease;
}

int32_t SkillHub::newId() {
  const int32_t id = m_next_id;
  m_next_id = m_next_id >= INT32_MAX ? 1 : m_next_id + 1;
  return id;
}

int32_t SkillHub::ask(logo::AskQ, const char*, uint32_t now) {
  if (m_server < 0) return -1;
  m_ask_id = newId();
  m_ask_ms = now;
  return m_ask_id;
}

bool SkillHub::answerFor(int fd, int32_t id, uint32_t now) const {
  return m_ask_id >= 0 && id == m_ask_id && fd == m_server && now - m_ask_ms <= kAskMs;
}

bool SkillHub::start(int32_t id, uint32_t now, int32_t* replaced) {
  if (m_server < 0 || id < 0) return false;
  if (replaced != nullptr) *replaced = m_lease_id;
  m_lease_id = id;
  m_last_drive_ms = now;
  return true;
}

bool SkillHub::endFor(int fd, int32_t id) const { return holder(fd, id); }

bool SkillHub::leaseOk(int fd, int32_t id, uint32_t now) {
  if (!holder(fd, id) || now - m_last_drive_ms > kDeadmanMs) return false;
  m_last_drive_ms = now;
  return true;
}

int32_t SkillHub::tick(uint32_t now) {
  if (m_ask_id >= 0 && now - m_ask_ms > kAskMs) m_ask_id = -1;
  if (m_lease_id < 0 || now - m_last_drive_ms <= kDeadmanMs) return -1;
  const int32_t lease = m_lease_id;
  m_lease_id = -1;
  return lease;
}

bool copyForCheck(const SkillHub& hub, logo::ThingNames& dst, const LockPort& lock) {
  dst.clear();
  if (!lock.take(lock.ctx, LockPort::kForever)) return false;  // not taken: nothing copied, nothing given back
  const bool ai = hub.server() >= 0;
  if (ai && !hub.clearPending()) dst.copyFrom(hub.things());  // clearPending: the old server's list is on its way out
  lock.give(lock.ctx);
  return ai;
}

}  // namespace core
}  // namespace cq
