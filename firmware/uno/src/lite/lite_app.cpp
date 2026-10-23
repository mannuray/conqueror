#include "drivers/board_config.h"
#include "domain/util/scheduler.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/battery_adc.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "ports/encoders.h"
#include "ports/imu.h"
#include "ports/ir.h"
#include "ports/led.h"
#include "ports/link.h"
#include "ports/motor.h"
#include "ports/servos.h"
#include "ports/tone.h"
#include "ports/ultrasonic.h"
#include "ports/watchdog.h"

namespace cq {
namespace lite {

namespace {
void linkWatchTask() {
  switch (g_lite.link.poll(port::clock::nowMs())) {
    case LinkChange::kLost: g_lite.events.push(proto::EventKind::kLinkLost); break;
    case LinkChange::kRestored: g_lite.events.push(proto::EventKind::kLinkRestored); break;
    case LinkChange::kNone: break;
  }
}

// LITE-TASKS-BEGIN
Task g_tasks[] = {
    {imuTask, 5000UL, 0UL, 0},
    {irTask, 5000UL, 0UL, 0},
    {linkWatchTask, 10000UL, 2500UL, 0},
    {controlTask, 10000UL, 2500UL, 0},
    {encodersTask, 10000UL, 5000UL, 0},
    {settingsTask, 20000UL, 7500UL, 0},
    {ultrasonicTask, 50000UL, 2500UL, 0},
    {telemetryFastDue, 20000UL, 0UL, 0},  // 50 Hz for the bridge motion service (spec 02 §E4)
    {buttonTask, 20000UL, 17500UL, 0},
    {batteryTask, 100000UL, 7500UL, 0},
    {selfTestRecoverTask, 100000UL, 7500UL, 0},
    {selfTestTask, 100000UL, 7500UL, 0},
    {cameraTask, 20000UL, 7500UL, 0},
    {uiTask, 20000UL, 17500UL, 0},
    {telemetrySlowDue, 1000000UL, 7500UL, 0},
};
// LITE-TASKS-END
Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));
}  // namespace

void setup() {
  paintStack();
  g_lite.reset_reason = port::watchdog::resetReason();
  port::link::begin(board::kBaud);
  port::motor::begin();
  port::encoders::begin();
  port::ultrasonic::begin();
  port::ir::begin();
  port::battery_adc::begin();
  port::button::begin();
  port::led::begin();
  port::tone::begin();
  port::servos::begin();
  settingsLoad();  // first: the saved camera centre is in the very first pulse (no jump at power-on)
  cameraTask();    // writes that pulse; Servo keeps it until attach
  port::servos::attach(Axis::kPan);
  port::servos::attach(Axis::kTilt);
  g_lite.imu_ok = port::imu::begin();
  port::ultrasonic::trigger();
  port::battery_adc::start();
  port::watchdog::enable();
  g_scheduler.start(port::clock::nowUs());
}

void loop() {
  const uint32_t t0 = port::clock::nowUs();
  commsPoll();
  g_scheduler.runDue(t0);
  txPump();
  port::watchdog::kick();
}

}  // namespace lite
}  // namespace cq
