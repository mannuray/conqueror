#include "app/app.h"
#include "ports/battery_adc.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

namespace cq {
namespace app {
namespace {

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

void setSlow(proto::SlowFlagBit bit, bool on) {
  const uint32_t m = 1UL << static_cast<uint8_t>(bit);
  g_app.slow_flags = on ? (g_app.slow_flags | m) : (g_app.slow_flags & ~m);
}

}  // namespace

void sensorsInit() {
  port::ultrasonic::begin();
  port::ir::begin();
  port::battery_adc::begin();
  port::button::begin();
  port::ultrasonic::trigger();
  port::battery_adc::start();
}

// 20 Hz: read the previous ping, trigger the next one (skipped while the echo is still high).
void ultrasonicTask() {
  uint16_t echo_us = 0;
  const EchoState st = port::ultrasonic::poll(echo_us);
  const uint32_t now = port::clock::nowMs();
  switch (g_app.us.onReading(st, echo_us, g_app.settings.global().ultrasonic_offset, now)) {
    case UltrasonicFilter::Change::kFault: g_app.events.push(proto::EventKind::kUsFault); break;
    case UltrasonicFilter::Change::kRecovered:
    case UltrasonicFilter::Change::kNone: break;
  }
  setFast(proto::FastFlagBit::kUsFault, g_app.us.fault());
  g_app.fast.dist_mm = g_app.us.distMm();
  g_app.fast.closing_cm_s = g_app.us.closingCmS();
  port::ultrasonic::trigger();
}

// 10 Hz battery.
void batteryTask() {
  uint16_t adc = 0;
  if (!port::battery_adc::ready(adc)) return;
  const GlobalSettings& gs = g_app.settings.global();
  const BatteryMonitor::Level lvl =
      g_app.batt.update(adc, gs.batt_mv_per_count, gs.batt_low, gs.batt_crit, gs.batt_hyst, port::clock::nowMs());
  if (g_app.batt.changed()) {
    if (lvl == BatteryMonitor::Level::kLow) g_app.events.push(proto::EventKind::kBattLow, g_app.batt.mv());
    if (lvl == BatteryMonitor::Level::kCritical) g_app.events.push(proto::EventKind::kBattCritical, g_app.batt.mv());
  }
  setSlow(proto::SlowFlagBit::kBattLow, lvl == BatteryMonitor::Level::kLow);
  setSlow(proto::SlowFlagBit::kBattCritical, lvl == BatteryMonitor::Level::kCritical);
  g_app.slow.batt_mv = g_app.batt.mv();
  port::battery_adc::start();
}

// 200 Hz: drain IR edges into the decoder.
void irTask() {
  uint32_t t_us = 0;
  IrEvent e{};
  while (port::ir::popEdge(t_us)) {
    if (g_app.nec.feed(t_us, port::clock::nowMs(), e)) {
      g_app.ir_event = e;
      g_app.ir_pending = true;
    }
  }
}

}  // namespace app
}  // namespace cq
