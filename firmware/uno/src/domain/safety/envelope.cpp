#include "domain/safety/envelope.h"

namespace cq {

proto::AckStatus Envelope::set(uint8_t speed_cap, uint8_t accel_cap, uint8_t lock_brake) {
  constexpr uint8_t kLock = 1u << static_cast<uint8_t>(proto::EnvelopeFlagBit::kLockBrake);
  constexpr uint8_t kChild = 1u << static_cast<uint8_t>(proto::EnvelopeFlagBit::kChildPlay);
  if (speed_cap > 100 || (lock_brake & static_cast<uint8_t>(~(kLock | kChild))) != 0) {
    return proto::AckStatus::kInvalidCommand;
  }
  m_speed_cap = speed_cap;
  m_accel_cap = accel_cap;
  m_child_play = (lock_brake & kChild) != 0;
  m_lock_brake = (lock_brake & kLock) != 0 || m_child_play;  // child play always locks the brake
  return proto::AckStatus::kOk;
}

proto::AckStatus Envelope::setBrake(bool on) {
  if (!on && m_lock_brake) return proto::AckStatus::kEnvelopeLocked;
  m_brake_on = on;
  return proto::AckStatus::kOk;
}

}  // namespace cq
