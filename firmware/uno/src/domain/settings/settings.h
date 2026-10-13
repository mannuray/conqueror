#pragma once
#include <stddef.h>
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Field names and types mirror protocol/defs/settings.yaml (fixed-point integers).
struct __attribute__((packed)) GlobalSettings {
  uint8_t encoders_enabled;
  uint16_t counts_per_rev;
  uint16_t counts_per_cm;      // ×0.01
  uint16_t motor_v_max;        // mV
  uint16_t accel_limit;        // %/s
  uint16_t turn_rate_max;      // °/s
  uint8_t pivot_fallback;
  uint8_t reverse_steer;       // 0 car, 1 tank
  uint8_t obstacle_stop;       // cm
  uint8_t obstacle_slow;       // cm
  uint8_t tilt_limit;          // °
  uint16_t bump_threshold;     // mg
  uint16_t deadman;            // ms
  uint16_t batt_low;           // mV
  uint16_t batt_crit;          // mV
  uint16_t batt_mv_per_count;  // ×0.01 mV
  uint16_t ir_handback;        // ms
  uint8_t parking_beeps;
  uint16_t cam_return_delay;   // ms
  uint8_t servo_idle_detach;
  uint8_t pan_min, pan_max, tilt_min, tilt_max;  // °
  int16_t pan_trim, tilt_trim;                   // ×0.1 °
  uint16_t servo_speed;        // °/s
  uint16_t servo_accel;        // °/s²
  int16_t gyro_bias_x, gyro_bias_y, gyro_bias_z;  // raw
  int16_t level_pitch, level_roll;                // ×0.01 °
  uint8_t ultrasonic_offset;   // mm
  uint8_t active_profile;
  uint8_t override_idle_s;     // s  (Safety Override, LLD §10)
  uint16_t override_max_s;     // s
  uint8_t override_tilt_deg;   // °
  uint8_t led_max;             // 0..255 WS2812 brightness cap
  uint16_t batt_hyst;          // mV recovery hysteresis (low and critical)
};
// 63 B: the global EEPROM block (len + data + CRC = 66 B at address 8) still ends before kEeProfileAddr = 80
// (static_assert in settings_saver.h). Saves are staged per block (SettingsSaver), so the writer queue only
// needs magic + global = 69 B of its 80 B.

struct __attribute__((packed)) ProfileSettings {
  uint8_t turn_rate_full_speed;  // %
  uint16_t track_width_eff;      // mm
  uint8_t deadband_start[4];     // l_fwd, l_rev, r_fwd, r_rev
  uint8_t deadband_keep[4];
  uint16_t max_cps_l, max_cps_r;
  int32_t pid_speed_kp, pid_speed_ki, pid_speed_kd;      // ×0.001
  int32_t pid_rate_kp, pid_rate_ki, pid_rate_kd;
  int32_t pid_heading_kp, pid_heading_ki, pid_heading_kd;
};

class Settings {
 public:
  static constexpr uint8_t kBlockGlobal = 0;
  static constexpr uint8_t kBlockProfile = 1;

  void loadDefaults();
  bool get(uint8_t id, int32_t& value) const;
  proto::AckStatus set(uint8_t id, int32_t value);
  const GlobalSettings& global() const { return m_global; }
  const ProfileSettings& profile() const { return m_profile; }
  uint8_t serialize(uint8_t block, uint8_t* out) const;
  bool deserialize(uint8_t block, const uint8_t* data, uint8_t len, uint16_t stored_crc);
  uint16_t crc() const;
  bool dirty() const { return m_dirty; }
  void clearDirty() { m_dirty = false; }

 private:
  bool slot(uint8_t id, uint8_t*& ptr, proto::WireType& type, proto::SettingDescriptor& desc) const;
  bool keepsOrder(uint8_t id, int32_t value) const;  // ordering invariants vs current values
  GlobalSettings m_global{};
  ProfileSettings m_profile{};
  bool m_dirty = false;
};

}  // namespace cq
