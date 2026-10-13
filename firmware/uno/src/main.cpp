#include <Arduino.h>

#include "domain/util/scheduler.h"
#include "drivers/board_config.h"
#include "ports/clock.h"
#include "ports/watchdog.h"

namespace {
void heartbeatTask() {}

cq::Task g_tasks[] = {
    {heartbeatTask, 1000000UL, 0},
};
cq::Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));
cq::LoadMeter g_load;
}  // namespace

void setup() {
  const cq::proto::ResetReason reason = cq::port::watchdog::resetReason();
  Serial.begin(cq::board::kBaud);
  Serial.print(F("CQ boot reset="));
  Serial.println(static_cast<uint8_t>(reason));
  cq::port::watchdog::enable();
  g_scheduler.start(cq::port::clock::nowUs());
}

void loop() {
  const uint32_t t0 = cq::port::clock::nowUs();
  if (g_scheduler.runDue(t0) > 0) {
    const uint32_t t1 = cq::port::clock::nowUs();
    g_load.add(t1 - t0, t1);
  }
  cq::port::watchdog::kick();
}
