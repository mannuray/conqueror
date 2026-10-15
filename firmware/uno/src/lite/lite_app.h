#pragma once
#include <stdint.h>

#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"
#include "generated/cq_protocol.h"

namespace cq {
namespace lite {

// All Lite state in one object (no heap). Later tasks add members before LITE-STATE-END.
struct LiteApp {
  FrameParser parser;
  FrameWriter writer;
  EventQueue events;
  AckQueue acks;
  RejectLimiter rejects;
  LinkSession link;
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
  bool fast_due = false;
  bool slow_due = false;
  proto::SettingValueMsg setting_value{};
  bool setting_value_pending = false;
  proto::ResetReason reset_reason = proto::ResetReason::kUnknown;
  bool encoders_enabled = false;
  uint16_t fast_flags = 0;
  uint32_t slow_flags = 0;
  // LITE-STATE-END
};
extern LiteApp g_lite;

void setup();
void loop();
void commsPoll();
void txPump();
void telemetryFastDue();
void telemetrySlowDue();

}  // namespace lite
}  // namespace cq
