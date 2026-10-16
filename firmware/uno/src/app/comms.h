#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

struct Route {
  uint8_t type;
  proto::AckStatus (*handler)(const uint8_t* payload, uint8_t len);
  bool needs_ack;
};

extern const Route kRoutes[];
extern const uint8_t kRouteCount;

void commsPoll();   // every loop: RX bytes → parser → route → ACK queue
void txPump();      // every loop: ACK > requests > EVENT > HELLO > CAL > FAST > SLOW
void telemetryFastDue();  // 50 Hz task: marks fast telemetry due
void telemetrySlowDue();  // 1 Hz task: fills health fields, marks slow due
uint16_t freeRam();

}  // namespace app
}  // namespace cq
