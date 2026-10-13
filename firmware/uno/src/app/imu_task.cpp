#include <math.h>

#include "app/app.h"
#include "domain/estimation/imu_scale.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/imu.h"

namespace cq {
namespace app {
namespace {

constexpr float kDt = 0.005f;
constexpr uint8_t kFaultStreak = 10;     // 50 ms of failed reads
constexpr uint32_t kReinitMs = 1000;     // begin() retry period while !imu_ok (<= ~3 ms per try)

uint32_t g_last_init_ms = 0;

void setFlag(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

}  // namespace

void imuInit() {
  g_last_init_ms = port::clock::nowMs();
  g_app.imu_ok = port::imu::begin();
  if (g_app.imu_ok) g_app.imu_fail_streak = 0;
  setFlag(proto::FastFlagBit::kImuFault, !g_app.imu_ok);
}

void imuTask() {
  const uint32_t now = port::clock::nowMs();
  if (!g_app.imu_ok) {
    // Absent or faulted IMU: retry begin() once per second, never at 200 Hz (a stuck bus costs
    // a 3 ms Wire timeout per transaction).
    if (elapsed(now, g_last_init_ms, kReinitMs)) imuInit();
    g_app.slow.err_i2c = port::imu::i2cErrors();
    return;
  }
  ImuRaw& r = g_app.imu_raw;
  if (!port::imu::read(r)) {
    if (g_app.imu_fail_streak < 255) ++g_app.imu_fail_streak;  // saturates: no wrap, no repeat event
    if (g_app.imu_fail_streak == kFaultStreak) {
      setFlag(proto::FastFlagBit::kImuFault, true);
      g_app.events.push(proto::EventKind::kImuFault);
      g_app.imu_ok = false;  // the 1 s re-init above takes over
      g_last_init_ms = now;
    }
    g_app.slow.err_i2c = port::imu::i2cErrors();
    return;
  }
  g_app.imu_fail_streak = 0;

  const GlobalSettings& gs = g_app.settings.global();
  const float ax = r.ax / imu_scale::kAccelLsbPerG;
  const float ay = r.ay / imu_scale::kAccelLsbPerG;
  const float az = r.az / imu_scale::kAccelLsbPerG;
  // float maths: int16 - int16 in 16-bit int would overflow for a saturated gyro with a bias
  const float gx = (static_cast<float>(r.gx) - gs.gyro_bias_x) / imu_scale::kGyroLsbPerDps;
  const float gy = (static_cast<float>(r.gy) - gs.gyro_bias_y) / imu_scale::kGyroLsbPerDps;
  const float gz = (static_cast<float>(r.gz) - gs.gyro_bias_z) / imu_scale::kGyroLsbPerDps;

  g_app.attitude.update(ax, ay, az, gx, gy, kDt);  // accel correction inside at 50 Hz
  g_app.yaw.update(gz, kDt, g_app.stationary);
  g_app.yaw_rate_dps = g_app.yaw.rateDps();
  g_app.accel_var.add(sqrtf(ax * ax + ay * ay + az * az), now);

  const float pitch = g_app.attitude.pitchDeg() - gs.level_pitch / 100.0f;
  const float roll = g_app.attitude.rollDeg() - gs.level_roll / 100.0f;

  int16_t mg = 0, dir = 0;
  if (g_app.bump.update(ax, ay, gs.bump_threshold, now, mg, dir)) {
    g_app.events.push(proto::EventKind::kBump, mg, dir);
    ++g_app.bump_seq;
  }

  switch (g_app.lift.update(az, pitch, roll, g_app.safety_override.tiltLimit(gs.tilt_limit, gs.override_tilt_deg), now)) {
    case LiftDetector::Change::kLifted: g_app.events.push(proto::EventKind::kLifted); break;
    case LiftDetector::Change::kLanded: g_app.events.push(proto::EventKind::kLanded); break;
    case LiftDetector::Change::kTilted:
      g_app.events.push(proto::EventKind::kTilted, static_cast<int16_t>(pitch * 10), static_cast<int16_t>(roll * 10));
      break;
    case LiftDetector::Change::kLevel: g_app.events.push(proto::EventKind::kLevel); break;
    case LiftDetector::Change::kNone: break;
  }
  setFlag(proto::FastFlagBit::kLifted, g_app.lift.lifted());
  setFlag(proto::FastFlagBit::kTilted, g_app.lift.tilted());

  proto::TelemFastMsg& f = g_app.fast;
  f.yaw = static_cast<uint16_t>(g_app.yaw.yawDeg() * 100.0f) % 36000u;
  f.yaw_rate = static_cast<int16_t>(clampf(gz * 10.0f, -32767.0f, 32767.0f));
  f.pitch = static_cast<int16_t>(pitch * 100.0f);
  f.roll = static_cast<int16_t>(roll * 100.0f);
  f.flags = g_app.fast_flags;
  g_app.slow.temp = imu_scale::tempCentiC(r.temp);
  g_app.slow.accel_var = g_app.accel_var.value();
  g_app.slow.err_i2c = port::imu::i2cErrors();
}

}  // namespace app
}  // namespace cq
