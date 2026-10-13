#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Safety envelope from the ESP32 (child-play etc.). The wire byte lock_brake is a bit set
// (proto::EnvelopeFlagBit): LOCK_BRAKE forces the obstacle brake on; CHILD_PLAY marks the child_play
// preset, implies the brake lock and is the only envelope that blocks Safety Override (LLD §10.1).
class Envelope {
 public:
  proto::AckStatus set(uint8_t speed_cap, uint8_t accel_cap, uint8_t lock_brake);
  proto::AckStatus setBrake(bool on);
  uint8_t speedCap() const { return m_speed_cap; }
  uint8_t accelCap() const { return m_accel_cap; }  // %/s, 0 = no cap
  bool lockBrake() const { return m_lock_brake; }
  bool childPlay() const { return m_child_play; }
  uint8_t flags() const {  // wire form, echoed in TELEM_SLOW.env_lock_brake / ENVELOPE_CHANGED b
    return static_cast<uint8_t>((m_lock_brake ? 1u << static_cast<uint8_t>(proto::EnvelopeFlagBit::kLockBrake) : 0u) |
                                (m_child_play ? 1u << static_cast<uint8_t>(proto::EnvelopeFlagBit::kChildPlay) : 0u));
  }
  bool brakeOn() const { return m_brake_on || m_lock_brake; }
  bool active() const { return m_speed_cap < 100 || m_accel_cap != 0 || m_lock_brake; }
  // LLD §9.7: calibration needs full authority, so any active envelope (child play included) refuses
  // a script to start and stops a running one.
  bool blocksCalibration() const { return active(); }

 private:
  uint8_t m_speed_cap = 100;
  uint8_t m_accel_cap = 0;
  bool m_lock_brake = false;
  bool m_child_play = false;
  bool m_brake_on = true;
};

}  // namespace cq
