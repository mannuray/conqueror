#include "domain/settings/settings.h"

#include <string.h>

#include "domain/settings/crc16.h"

// PROGMEM itself comes from generated/cq_protocol.h (defined empty on host).
#ifdef CQ_HOST_TEST
#define CQ_READ_P(dst, src, n) memcpy((dst), (src), (n))
#else
#include <avr/pgmspace.h>
#define CQ_READ_P(dst, src, n) memcpy_P((dst), (src), (n))
#endif

namespace cq {
namespace {

struct Slot {
  uint8_t id;
  uint8_t block;
  uint8_t offset;
};

#define G(field, sid) {static_cast<uint8_t>(proto::SettingId::sid), Settings::kBlockGlobal, \
                       static_cast<uint8_t>(offsetof(GlobalSettings, field))}
#define P(field, sid) {static_cast<uint8_t>(proto::SettingId::sid), Settings::kBlockProfile, \
                       static_cast<uint8_t>(offsetof(ProfileSettings, field))}

const Slot kSlots[] PROGMEM = {
    G(encoders_enabled, kEncodersEnabled), G(counts_per_rev, kCountsPerRev),
    G(counts_per_cm, kCountsPerCm), G(motor_v_max, kMotorVMax), G(accel_limit, kAccelLimit),
    G(turn_rate_max, kTurnRateMax), P(turn_rate_full_speed, kTurnRateFullSpeed),
    P(track_width_eff, kTrackWidthEff), G(pivot_fallback, kPivotFallback),
    G(reverse_steer, kReverseSteer), G(obstacle_stop, kObstacleStop), G(obstacle_slow, kObstacleSlow),
    G(tilt_limit, kTiltLimit), G(bump_threshold, kBumpThreshold), G(deadman, kDeadman),
    G(batt_low, kBattLow), G(batt_crit, kBattCrit), G(batt_mv_per_count, kBattMvPerCount),
    G(ir_handback, kIrHandback), G(parking_beeps, kParkingBeeps),
    G(cam_return_delay, kCamReturnDelay), G(servo_idle_detach, kServoIdleDetach),
    G(pan_min, kPanMin), G(pan_max, kPanMax), G(tilt_min, kTiltMin), G(tilt_max, kTiltMax),
    G(pan_trim, kPanTrim), G(tilt_trim, kTiltTrim), G(servo_speed, kServoSpeed),
    G(servo_accel, kServoAccel), G(gyro_bias_x, kGyroBiasX), G(gyro_bias_y, kGyroBiasY),
    G(gyro_bias_z, kGyroBiasZ), G(level_pitch, kLevelPitch), G(level_roll, kLevelRoll),
    G(ultrasonic_offset, kUltrasonicOffset), G(active_profile, kActiveProfile),
    G(override_idle_s, kOverrideIdleS), G(override_max_s, kOverrideMaxS), G(override_tilt_deg, kOverrideTiltDeg),
    G(led_max, kLedMax), G(batt_hyst, kBattHyst), G(track_balance, kTrackBalance),
    P(deadband_start[0], kDeadbandStartLFwd), P(deadband_start[1], kDeadbandStartLRev),
    P(deadband_start[2], kDeadbandStartRFwd), P(deadband_start[3], kDeadbandStartRRev),
    P(deadband_keep[0], kDeadbandKeepLFwd), P(deadband_keep[1], kDeadbandKeepLRev),
    P(deadband_keep[2], kDeadbandKeepRFwd), P(deadband_keep[3], kDeadbandKeepRRev),
    P(max_cps_l, kMaxCpsL), P(max_cps_r, kMaxCpsR),
    P(pid_speed_kp, kPidSpeedKp), P(pid_speed_ki, kPidSpeedKi), P(pid_speed_kd, kPidSpeedKd),
    P(pid_rate_kp, kPidRateKp), P(pid_rate_ki, kPidRateKi), P(pid_rate_kd, kPidRateKd),
    P(pid_heading_kp, kPidHeadingKp), P(pid_heading_ki, kPidHeadingKi),
    P(pid_heading_kd, kPidHeadingKd),
};
#undef G
#undef P

// Ordering invariants between settings: lo <= hi, or lo < hi when strict.
struct Order {
  uint8_t lo;
  uint8_t hi;
  bool strict;
};

const Order kOrders[] PROGMEM = {
    {static_cast<uint8_t>(proto::SettingId::kPanMin), static_cast<uint8_t>(proto::SettingId::kPanMax), false},
    {static_cast<uint8_t>(proto::SettingId::kTiltMin), static_cast<uint8_t>(proto::SettingId::kTiltMax), false},
    {static_cast<uint8_t>(proto::SettingId::kObstacleStop), static_cast<uint8_t>(proto::SettingId::kObstacleSlow),
     true},
    {static_cast<uint8_t>(proto::SettingId::kBattCrit), static_cast<uint8_t>(proto::SettingId::kBattLow), true},
};

bool inOrder(int32_t lo, int32_t hi, bool strict) { return strict ? lo < hi : lo <= hi; }

uint8_t wireSize(proto::WireType t) {
  switch (t) {
    case proto::WireType::kU8:
    case proto::WireType::kI8: return 1;
    case proto::WireType::kU16:
    case proto::WireType::kI16: return 2;
    default: return 4;
  }
}

int32_t loadValue(const uint8_t* p, proto::WireType t) {
  switch (t) {
    case proto::WireType::kU8: return p[0];
    case proto::WireType::kI8: return static_cast<int8_t>(p[0]);
    case proto::WireType::kU16: return static_cast<uint16_t>(p[0] | (p[1] << 8));
    case proto::WireType::kI16: return static_cast<int16_t>(p[0] | (p[1] << 8));
    case proto::WireType::kU32:
    case proto::WireType::kI32: {
      int32_t v = 0;
      memcpy(&v, p, 4);
      return v;
    }
  }
  return 0;
}

void storeValue(uint8_t* p, proto::WireType t, int32_t v) {
  const uint8_t n = wireSize(t);
  for (uint8_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}

bool findDescriptor(uint8_t id, proto::SettingDescriptor& out) {
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {
    CQ_READ_P(&out, &proto::kSettingDescriptors[i], sizeof out);
    if (out.id == id) return true;
  }
  return false;
}

}  // namespace

bool Settings::slot(uint8_t id, uint8_t*& ptr, proto::WireType& type,
                    proto::SettingDescriptor& desc) const {
  if (!findDescriptor(id, desc)) return false;
  for (uint8_t i = 0; i < sizeof kSlots / sizeof kSlots[0]; ++i) {
    Slot s;
    CQ_READ_P(&s, &kSlots[i], sizeof s);
    if (s.id != id) continue;
    uint8_t* base = s.block == kBlockGlobal ? reinterpret_cast<uint8_t*>(const_cast<GlobalSettings*>(&m_global))
                                            : reinterpret_cast<uint8_t*>(const_cast<ProfileSettings*>(&m_profile));
    ptr = base + s.offset;
    type = desc.type;
    return true;
  }
  return false;
}

void Settings::loadDefaults() {
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {
    proto::SettingDescriptor d;
    CQ_READ_P(&d, &proto::kSettingDescriptors[i], sizeof d);
    uint8_t* p = nullptr;
    proto::WireType t{};
    proto::SettingDescriptor dd;
    if (slot(d.id, p, t, dd)) storeValue(p, t, d.def);
  }
  m_dirty = false;
}

bool Settings::get(uint8_t id, int32_t& value) const {
  uint8_t* p = nullptr;
  proto::WireType t{};
  proto::SettingDescriptor d;
  if (!slot(id, p, t, d)) return false;
  value = loadValue(p, t);
  return true;
}

proto::AckStatus Settings::set(uint8_t id, int32_t value) {
  uint8_t* p = nullptr;
  proto::WireType t{};
  proto::SettingDescriptor d;
  if (!slot(id, p, t, d)) return proto::AckStatus::kSettingUnknown;
  if (value < d.min || value > d.max) return proto::AckStatus::kSettingOutOfRange;
  if (!keepsOrder(id, value)) return proto::AckStatus::kSettingOutOfRange;
  storeValue(p, t, value);
  m_dirty = true;
  ++m_changes;
  return proto::AckStatus::kOk;
}

bool Settings::keepsOrder(uint8_t id, int32_t value) const {
  for (uint8_t i = 0; i < sizeof kOrders / sizeof kOrders[0]; ++i) {
    Order o;
    CQ_READ_P(&o, &kOrders[i], sizeof o);
    int32_t other = 0;
    if (id == o.lo && get(o.hi, other) && !inOrder(value, other, o.strict)) return false;
    if (id == o.hi && get(o.lo, other) && !inOrder(other, value, o.strict)) return false;
  }
  return true;
}

uint8_t Settings::serialize(uint8_t block, uint8_t* out) const {
  if (block == kBlockGlobal) {
    memcpy(out, &m_global, sizeof m_global);
    return sizeof m_global;
  }
  memcpy(out, &m_profile, sizeof m_profile);
  return sizeof m_profile;
}

bool Settings::deserialize(uint8_t block, const uint8_t* data, uint8_t len, uint16_t stored_crc) {
  const uint8_t expected = block == kBlockGlobal ? sizeof m_global : sizeof m_profile;
  if (len != expected || crc16(data, len) != stored_crc) return false;
  if (block == kBlockGlobal) {
    memcpy(&m_global, data, len);
  } else {
    memcpy(&m_profile, data, len);
  }
  return true;
}

uint16_t Settings::crc() const {
  const uint16_t c = crc16(reinterpret_cast<const uint8_t*>(&m_global), sizeof m_global);
  return crc16(reinterpret_cast<const uint8_t*>(&m_profile), sizeof m_profile, c);
}

}  // namespace cq
