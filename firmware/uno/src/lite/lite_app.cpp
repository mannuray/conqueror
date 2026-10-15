#include "drivers/board_config.h"
#include "domain/util/scheduler.h"
#include "lite/lite_app.h"
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

// Skeleton router (replaced by lite_routes.cpp in Task 11): link frames only; nothing else is acked.
bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st) {
  (void)st;
  switch (static_cast<proto::FrameType>(type)) {
    case proto::FrameType::kHelloAck: {
      proto::HelloAckMsg m{};
      if (proto::decode(p, len, m)) g_lite.link.onHelloAck();
      return false;
    }
    case proto::FrameType::kHelloReq: g_lite.link.onHelloReq(); return false;
    default: ++g_lite.parser.errors().unknown_type; return false;
  }
}

namespace {
void linkWatchTask() {
  switch (g_lite.link.poll(port::clock::nowMs())) {
    case LinkChange::kLost: g_lite.events.push(proto::EventKind::kLinkLost); break;
    case LinkChange::kRestored: g_lite.events.push(proto::EventKind::kLinkRestored); break;
    case LinkChange::kNone: break;
  }
}
void motorsZeroTask() { port::motor::apply(PwmOutput::zero()); }

// LITE-TASKS-BEGIN
Task g_tasks[] = {
    {linkWatchTask, 10000UL, 2500UL, 0},
    {motorsZeroTask, 10000UL, 2500UL, 0},
    {telemetryFastDue, 50000UL, 0UL, 0},
    {telemetrySlowDue, 1000000UL, 7500UL, 0},
};
// LITE-TASKS-END
Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));
}  // namespace

void setup() {
  g_lite.reset_reason = port::watchdog::resetReason();
  port::link::begin(board::kBaud);
  port::motor::begin();
  port::imu::begin();
  port::encoders::begin();
  port::ultrasonic::begin();
  port::ir::begin();
  port::battery_adc::begin();
  port::button::begin();
  port::led::begin();
  port::tone::begin();
  port::servos::begin();
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
