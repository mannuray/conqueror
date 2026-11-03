#include "core/skill_hub.h"

namespace cq {
namespace core {

SkillHub::Decision SkillHub::onHello(int fd, const ThingList& t) {
  if (m_server >= 0 && m_server != fd) return Decision::kBusy;
  if (!m_things.set(t.word, t.n_names, t.names, t.n)) return Decision::kTooBig;
  m_server = fd;
  return Decision::kOk;
}

SkillHub::Decision SkillHub::onThings(int fd, const ThingList& t) {
  if (fd != m_server || m_server < 0) return Decision::kNotServer;
  return m_things.set(t.word, t.n_names, t.names, t.n) ? Decision::kOk : Decision::kTooBig;
}

int32_t SkillHub::onClose(int fd) {
  if (fd < 0 || fd != m_server) return -1;
  const int32_t lease = m_lease_id;
  m_server = -1;
  m_things.clear();
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

bool SkillHub::answerFor(int fd, int32_t id) const {
  return m_ask_id >= 0 && id == m_ask_id && fd == m_server;
}

bool SkillHub::start(int32_t id, uint32_t now) {
  if (m_server < 0 || id < 0) return false;
  m_lease_id = id;
  m_lease_fd = m_server;
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

}  // namespace core
}  // namespace cq
