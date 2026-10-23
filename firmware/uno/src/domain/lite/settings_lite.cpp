#include "domain/lite/settings_lite.h"

#include <stddef.h>
#include <string.h>

namespace cq {

namespace {
using Id = proto::SettingId;
bool inRange(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }
// The block sizes saved so far: before plan J, before the camera trims, now. Older blocks end early.
bool knownSize(uint8_t n) {
  return n == sizeof(LiteSettings) || n == offsetof(LiteSettings, track_balance) || n == offsetof(LiteSettings, pan_trim);
}
}  // namespace

void SettingsLite::defaults() {
  m_s.gyro_bias_z = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    m_s.db_start[i] = 110;
    m_s.db_keep[i] = 90;
  }
  m_s.heading_kp_milli = 3500;
  m_s.rate_kp_milli = 1200;
  m_s.accel_limit = 200;
  m_s.turn_full = 40;
  m_s.obstacle_stop = 15;
  m_s.obstacle_slow = 50;
  m_s.reverse_steer = 0;
  m_s.encoders_enabled = 0;
  m_s.track_balance = 0;
  m_s.pan_trim = 0;
  m_s.tilt_trim = 0;
}

bool SettingsLite::load(uint8_t (*read)(uint16_t)) {
  defaults();
  const uint8_t stored = read(kAddr + 2);
  if (read(kAddr) != kMagic0 || read(kAddr + 1) != kMagic1 || !knownSize(stored)) {
    return false;
  }
  uint8_t buf[sizeof(LiteSettings) + 1];
  buf[0] = stored;
  for (uint8_t i = 0; i < stored; ++i) buf[i + 1] = read(static_cast<uint16_t>(kAddr + 3 + i));
  if (proto::crc8(buf, static_cast<uint8_t>(stored + 1)) != read(static_cast<uint16_t>(kAddr + 3 + stored))) return false;
  // Plan E deferred minor: every loaded value passes the same range check as SETTING_SET, else defaults.
  SettingsLite loaded;
  loaded.defaults();  // fields not in an older block keep their defaults (plan J)
  memcpy(&loaded.m_s, buf + 1, stored);
  for (uint16_t id = 0; id < 256; ++id) {  // every id the Lite knows (no table: RAM is tight)
    int32_t v = 0;
    const uint8_t i8 = static_cast<uint8_t>(id);
    if (loaded.get(i8, v) && set(i8, v) != proto::AckStatus::kOk) {
      defaults();
      return false;
    }
  }
  m_s = loaded.m_s;
  return true;
}

uint8_t SettingsLite::encode(uint8_t* out) const {
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = sizeof(LiteSettings);
  memcpy(out + 3, &m_s, sizeof(LiteSettings));
  out[3 + sizeof(LiteSettings)] = proto::crc8(out + 2, static_cast<uint8_t>(sizeof(LiteSettings) + 1));
  return kBlockSize;
}

bool SettingsLite::get(uint8_t id, int32_t& v) const {
  const uint8_t db = static_cast<uint8_t>(Id::kDeadbandStartLFwd);
  const uint8_t dk = static_cast<uint8_t>(Id::kDeadbandKeepLFwd);
  if (id >= db && id < db + 4) { v = m_s.db_start[id - db]; return true; }
  if (id >= dk && id < dk + 4) { v = m_s.db_keep[id - dk]; return true; }
  switch (static_cast<Id>(id)) {
    case Id::kGyroBiasZ: v = m_s.gyro_bias_z; return true;
    case Id::kPidHeadingKp: v = m_s.heading_kp_milli; return true;
    case Id::kPidRateKp: v = m_s.rate_kp_milli; return true;
    case Id::kAccelLimit: v = m_s.accel_limit; return true;
    case Id::kTurnRateFullSpeed: v = m_s.turn_full; return true;
    case Id::kObstacleStop: v = m_s.obstacle_stop; return true;
    case Id::kObstacleSlow: v = m_s.obstacle_slow; return true;
    case Id::kReverseSteer: v = m_s.reverse_steer; return true;
    case Id::kEncodersEnabled: v = m_s.encoders_enabled; return true;
    case Id::kTrackBalance: v = m_s.track_balance; return true;
    case Id::kPanTrim: v = m_s.pan_trim; return true;
    case Id::kTiltTrim: v = m_s.tilt_trim; return true;
    default: return false;
  }
}

proto::AckStatus SettingsLite::set(uint8_t id, int32_t v) {
  constexpr proto::AckStatus kOk = proto::AckStatus::kOk;
  constexpr proto::AckStatus kRange = proto::AckStatus::kSettingOutOfRange;
  const uint8_t db = static_cast<uint8_t>(Id::kDeadbandStartLFwd);
  const uint8_t dk = static_cast<uint8_t>(Id::kDeadbandKeepLFwd);
  if (id >= db && id < db + 4) {
    if (!inRange(v, 0, 255)) return kRange;
    m_s.db_start[id - db] = static_cast<uint8_t>(v);
    return kOk;
  }
  if (id >= dk && id < dk + 4) {
    if (!inRange(v, 0, 255)) return kRange;
    m_s.db_keep[id - dk] = static_cast<uint8_t>(v);
    return kOk;
  }
  switch (static_cast<Id>(id)) {
    case Id::kGyroBiasZ: if (!inRange(v, -2000, 2000)) return kRange; m_s.gyro_bias_z = static_cast<int16_t>(v); return kOk;
    case Id::kPidHeadingKp: if (!inRange(v, 0, 60000)) return kRange; m_s.heading_kp_milli = static_cast<uint16_t>(v); return kOk;
    case Id::kPidRateKp: if (!inRange(v, 0, 60000)) return kRange; m_s.rate_kp_milli = static_cast<uint16_t>(v); return kOk;
    case Id::kAccelLimit: if (!inRange(v, 10, 1000)) return kRange; m_s.accel_limit = static_cast<uint16_t>(v); return kOk;
    case Id::kTurnRateFullSpeed: if (!inRange(v, 10, 100)) return kRange; m_s.turn_full = static_cast<uint8_t>(v); return kOk;
    case Id::kObstacleStop: if (!inRange(v, 5, 100)) return kRange; m_s.obstacle_stop = static_cast<uint8_t>(v); return kOk;
    case Id::kObstacleSlow: if (!inRange(v, 10, 200)) return kRange; m_s.obstacle_slow = static_cast<uint8_t>(v); return kOk;
    case Id::kReverseSteer: if (!inRange(v, 0, 1)) return kRange; m_s.reverse_steer = static_cast<uint8_t>(v); return kOk;
    case Id::kEncodersEnabled: if (!inRange(v, 0, 1)) return kRange; m_s.encoders_enabled = static_cast<uint8_t>(v); return kOk;
    case Id::kTrackBalance: if (!inRange(v, -100, 100)) return kRange; m_s.track_balance = static_cast<int16_t>(v); return kOk;
    case Id::kPanTrim: if (!inRange(v, -300, 300)) return kRange; m_s.pan_trim = static_cast<int16_t>(v); return kOk;
    case Id::kTiltTrim: if (!inRange(v, -300, 300)) return kRange; m_s.tilt_trim = static_cast<int16_t>(v); return kOk;
    default: return proto::AckStatus::kUnavailable;
  }
}

}  // namespace cq
