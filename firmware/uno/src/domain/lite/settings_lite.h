#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct LiteSettings {
  int16_t gyro_bias_z;
  uint8_t db_start[4];  // l_fwd, l_rev, r_fwd, r_rev
  uint8_t db_keep[4];
  uint16_t heading_kp_milli;
  uint16_t rate_kp_milli;
  uint16_t accel_limit;
  uint8_t turn_full;
  uint8_t obstacle_stop;
  uint8_t obstacle_slow;
  uint8_t reverse_steer;
  uint8_t encoders_enabled;
  int16_t track_balance;  // ‰, spec 02 §F5.1 (plan J; older EEPROM blocks end before it)
};

// The Lite settings (spec 02 Part D): existing SettingId numbers, so the bridge and bench tools work unchanged.
// EEPROM block at kAddr: magic "L1" (2 B), payload size (1 B), payload, CRC-8 of size + payload.
// Any other id → UNAVAILABLE.
class SettingsLite {
 public:
  static constexpr uint16_t kAddr = 0;
  static constexpr uint8_t kMagic0 = 'L';
  static constexpr uint8_t kMagic1 = '1';
  static constexpr uint8_t kBlockSize = static_cast<uint8_t>(4 + sizeof(LiteSettings));
  void defaults();
  bool load(uint8_t (*read)(uint16_t addr));  // false → defaults in use
  bool get(uint8_t id, int32_t& v) const;
  proto::AckStatus set(uint8_t id, int32_t v);
  uint8_t encode(uint8_t* out) const;
  const LiteSettings& values() const { return m_s; }

 private:
  LiteSettings m_s{};
};

}  // namespace cq
