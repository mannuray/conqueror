#include "domain/input/boot_button.h"
#include "drivers/board_config.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/battery_adc.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "ports/eeprom.h"
#include "ports/encoders.h"
#include "ports/imu.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

namespace cq {
namespace lite {

namespace {
constexpr uint8_t kFaultStreak = 10;
constexpr uint32_t kReinitMs = 1000;
constexpr uint8_t kTiltLimitDeg = 45;    // full default (setting 13)
constexpr uint16_t kBattMvPerCountX100 = 4050, kBattLow = 7200, kBattCrit = 6600, kBattHyst = 200;
uint32_t g_last_init_ms = 0;
BootButton g_button;

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_lite.fast_flags = on ? static_cast<uint16_t>(g_lite.fast_flags | m) : static_cast<uint16_t>(g_lite.fast_flags & ~m);
}
void setSlow(proto::SlowFlagBit bit, bool on) {
  const uint32_t m = 1UL << static_cast<uint8_t>(bit);
  g_lite.slow_flags = on ? (g_lite.slow_flags | m) : (g_lite.slow_flags & ~m);
}
uint8_t eeRead(uint16_t a) { return port::eeprom::read(a); }
}  // namespace

bool imuUsable() { return g_lite.imu_ok && g_lite.imu_fail_streak < kFaultStreak; }

namespace {
// IMU absent or faulted: retry begin() once per second, never at 200 Hz (a stuck bus costs a timeout per try).
void imuRetry(uint32_t now) {
  if (static_cast<uint32_t>(now - g_last_init_ms) >= kReinitMs) {
    g_last_init_ms = now;
    g_lite.imu_ok = port::imu::begin();
    if (g_lite.imu_ok) g_lite.imu_fail_streak = 0;
  }
  setFast(proto::FastFlagBit::kImuFault, !g_lite.imu_ok);
}

void imuReadFailed(uint32_t now) {
  if (g_lite.imu_fail_streak < 255) ++g_lite.imu_fail_streak;
  if (g_lite.imu_fail_streak == kFaultStreak) {
    g_lite.events.push(proto::EventKind::kImuFault);
    g_lite.imu_ok = false;
    g_last_init_ms = now;
    setFast(proto::FastFlagBit::kImuFault, true);
  }
}

void tiltEvents(TiltLite::Change c) {
  switch (c) {
    case TiltLite::Change::kLifted: g_lite.events.push(proto::EventKind::kLifted); break;
    case TiltLite::Change::kLanded: g_lite.events.push(proto::EventKind::kLanded); break;
    case TiltLite::Change::kTilted: g_lite.events.push(proto::EventKind::kTilted); break;
    case TiltLite::Change::kLevel: g_lite.events.push(proto::EventKind::kLevel); break;
    case TiltLite::Change::kNone: break;
  }
  setFast(proto::FastFlagBit::kLifted, g_lite.tilt.lifted());
  setFast(proto::FastFlagBit::kTilted, g_lite.tilt.tilted());
}
}  // namespace

void imuTask() {
  const uint32_t now = port::clock::nowMs();
  if (!g_lite.imu_ok) {
    imuRetry(now);
    return;
  }
  ImuRaw& r = g_lite.imu_raw;
  if (!port::imu::read(r)) {
    imuReadFailed(now);
    return;
  }
  g_lite.imu_fail_streak = 0;
  const bool quiet = !g_lite.moving && !g_lite.tilt.lifted() && !g_lite.tilt.tilted() && !g_lite.self_test_pending;
  g_lite.gyro_zero.update(r.gz, quiet, now);
  // int32 then saturate: on the AVR int is 16-bit and a saturated reading minus the zero would wrap.
  int32_t gz = static_cast<int32_t>(r.gz) - g_lite.gyro_zero.zero();
  if (gz > INT16_MAX) gz = INT16_MAX;
  if (gz < INT16_MIN) gz = INT16_MIN;
  g_lite.yaw.update(static_cast<int16_t>(gz), !g_lite.moving);
  tiltEvents(g_lite.tilt.update(r.ax, r.ay, r.az, kTiltLimitDeg, now));
  g_lite.fast.yaw = static_cast<uint16_t>(g_lite.yaw.yawCdeg());
  g_lite.fast.yaw_rate = g_lite.yaw.rateDdps();
}

void irTask() {
  uint32_t t_us = 0;
  IrEvent e{};
  while (port::ir::popEdge(t_us)) {
    if (g_lite.nec.feed(t_us, port::clock::nowMs(), e)) {
      g_lite.ir_event = e;
      g_lite.ir_pending = true;
    }
  }
}

void ultrasonicTask() {
  uint16_t echo_us = 0;
  const EchoState st = port::ultrasonic::poll(echo_us);
  if (g_lite.range.onReading(st, echo_us, 0) == RangeLite::Change::kFault) g_lite.events.push(proto::EventKind::kUsFault);
  setFast(proto::FastFlagBit::kUsFault, g_lite.range.fault());
  g_lite.fast.dist_mm = g_lite.range.distMm();
  port::ultrasonic::trigger();
}

void batteryTask() {
  uint16_t adc = 0;
  if (!port::battery_adc::ready(adc)) return;
  const BatteryMonitor::Level lvl =
      g_lite.batt.update(adc, kBattMvPerCountX100, kBattLow, kBattCrit, kBattHyst, port::clock::nowMs());
  if (g_lite.batt.changed()) {
    if (lvl == BatteryMonitor::Level::kLow) g_lite.events.push(proto::EventKind::kBattLow, g_lite.batt.mv());
    if (lvl == BatteryMonitor::Level::kCritical) g_lite.events.push(proto::EventKind::kBattCritical, g_lite.batt.mv());
  }
  setSlow(proto::SlowFlagBit::kBattLow, lvl == BatteryMonitor::Level::kLow);
  setSlow(proto::SlowFlagBit::kBattCritical, lvl == BatteryMonitor::Level::kCritical);
  g_lite.slow.batt_mv = g_lite.batt.mv();
  port::battery_adc::start();
}

void encodersTask() {
  EncoderSnapshot s{};
  port::encoders::snapshot(s);
  g_lite.enc_l.update(s.count_l, s.period_l_us, s.last_edge_l_us, s.t_us);
  g_lite.enc_r.update(s.count_r, s.period_r_us, s.last_edge_r_us, s.t_us);
  g_lite.fast.enc_l = g_lite.enc_l.count();
  g_lite.fast.enc_r = g_lite.enc_r.count();
}

void buttonTask() {
  switch (g_button.update(port::button::pressed(), port::clock::nowMs())) {
    case BootButton::Action::kPress: g_lite.events.push(proto::EventKind::kButtonPress); break;
    case BootButton::Action::kWifiReset: g_lite.link.setRequest(static_cast<uint8_t>(proto::FrameType::kWifiResetReq)); break;
    case BootButton::Action::kAdminReset: g_lite.link.setRequest(static_cast<uint8_t>(proto::FrameType::kAdminResetReq)); break;
    default: break;
  }
}

// Boot self-test 2 s after reset (LLD §10.9 rules; reused SelfTest).
void selfTestTask() {
  if (!g_lite.self_test_pending || port::clock::nowMs() < 2000) return;
  g_lite.self_test_pending = false;
  g_lite.self_test.begin();
  g_lite.self_test.report(SelfTest::Check::kImu, g_lite.imu_ok);
  g_lite.self_test.report(SelfTest::Check::kUltrasonic, !g_lite.range.fault());
  g_lite.self_test.report(SelfTest::Check::kBattery, SelfTest::batteryPlausible(g_lite.batt.mv()));
  g_lite.self_test.report(SelfTest::Check::kSettings, g_lite.settings_loaded);
  g_lite.self_test.complete();
  g_lite.events.push(proto::EventKind::kSelftest, g_lite.self_test.failMask(), g_lite.self_test.locked() ? 1 : 0);
  setSlow(proto::SlowFlagBit::kSelftestLocked, g_lite.self_test.locked());
}

void selfTestRecoverTask() {
  if (g_lite.self_test.tickBattery(SelfTest::batteryPlausible(g_lite.batt.mv()), port::clock::nowMs())) {
    g_lite.events.push(proto::EventKind::kSelftest, g_lite.self_test.failMask(), g_lite.self_test.locked() ? 1 : 0);
    setSlow(proto::SlowFlagBit::kSelftestLocked, g_lite.self_test.locked());
  }
}

void settingsLoad() {
  g_lite.settings_loaded = g_lite.settings.load(eeRead);
  if (!g_lite.settings_loaded) g_lite.events.push(proto::EventKind::kSettingsReset);
  applySettings();
}

void settingsTask() {
  static const EepromIo kIo{port::eeprom::read, port::eeprom::writeByteAsync, port::eeprom::busy};
  if (g_lite.save_requested && !g_lite.moving && g_lite.eewriter.idle()) {
    uint8_t block[SettingsLite::kBlockSize];
    g_lite.settings.encode(block);
    if (g_lite.eewriter.enqueue(SettingsLite::kAddr, block, SettingsLite::kBlockSize)) {
      g_lite.save_requested = false;
      g_lite.events.push(proto::EventKind::kSettingsSaved);
    }
  }
  g_lite.eewriter.step(kIo);
}

}  // namespace lite
}  // namespace cq
