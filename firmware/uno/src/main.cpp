#include <Arduino.h>

#if defined(PROFILE_LITE)
// Uno Lite firmware (spec 02 Part D, docs/lld/uno-lite-lld.md).
#include "lite/lite_app.h"
void setup() { cq::lite::setup(); }
void loop() { cq::lite::loop(); }
#else
#include "app/app.h"
#include "app/comms.h"
#include "app/tasks.h"
#include "domain/util/scheduler.h"
#include "drivers/board_config.h"
#include "ports/clock.h"
#include "ports/link.h"
#include "ports/watchdog.h"

namespace {

void linkWatchTask() {
  // poll() runs onLinkLost() on a loss; the first contact after boot reports kNone.
  switch (cq::g_app.link.poll(cq::port::clock::nowMs())) {
    case cq::LinkChange::kLost:
      cq::g_app.events.push(cq::proto::EventKind::kLinkLost);
      break;
    case cq::LinkChange::kRestored:
      cq::g_app.events.push(cq::proto::EventKind::kLinkRestored);
      break;
    case cq::LinkChange::kNone:
      break;
  }
}

// TASKS-BEGIN (later tasks add rows before TASKS-END)
// {fn, period, phase, 0}. Phases keep the heavy tasks in separate passes (LLD §11.5): IMU on the
// 5 ms grid, control 2.5 ms after it, slow telemetry/camera at 7.5 ms, UI at 17.5 ms. With every
// phase at 0 all 14 tasks met once a second in one ~6 ms pass.
cq::Task g_tasks[] = {
    {cq::app::imuTask, 5000UL, 0UL, 0},
    {cq::app::controlTask, 10000UL, 2500UL, 0},
    {linkWatchTask, 10000UL, 2500UL, 0},
    {cq::app::telemetryFastDue, 20000UL, 0UL, 0},  // 50 Hz (spec 02 §E4)
    {cq::app::telemetrySlowDue, 1000000UL, 7500UL, 0},
    {cq::app::settingsTask, 20000UL, 7500UL, 0},
    {cq::app::irTask, 5000UL, 0UL, 0},
    {cq::app::ultrasonicTask, 50000UL, 2500UL, 0},
    {cq::app::batteryTask, 100000UL, 0UL, 0},
    {cq::app::selfTestTask, 1000000UL, 0UL, 0},
    {cq::app::selfTestRecoverTask, 100000UL, 0UL, 0},
    {cq::app::cameraTask, 20000UL, 7500UL, 0},
    {cq::app::uiTask, 20000UL, 17500UL, 0},
    {cq::app::inputTask, 20000UL, 17500UL, 0},
    // TASKS-END
};
cq::Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));

}  // namespace

void setup() {
  cq::g_app.reset_reason = cq::port::watchdog::resetReason();
  cq::port::link::begin(cq::board::kBaud);
  // SETUP-INIT (later tasks add driver/domain init before SETUP-END)
  cq::app::controlInit();
  cq::app::settingsLoad();
  cq::app::imuInit();
  cq::app::sensorsInit();
  cq::app::cameraInit();
  cq::app::uiInit();
  // SETUP-END
  cq::port::watchdog::enable();
  g_scheduler.start(cq::port::clock::nowUs());
}

void loop() {
  const uint32_t t0 = cq::port::clock::nowUs();
  cq::app::commsPoll();
  const uint8_t ran = g_scheduler.runDue(t0);
  cq::app::txPump();
  const uint32_t t1 = cq::port::clock::nowUs();
  if (ran > 0) cq::g_app.load.add(t1 - t0, t1);
  cq::g_app.scheduler_overruns = g_scheduler.overruns();
  cq::port::watchdog::kick();
}
#endif  // PROFILE_LITE
