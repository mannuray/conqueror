#pragma once
#include <stdint.h>

#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"
#include "domain/settings/eeprom_writer.h"
#include "domain/settings/settings.h"
#include "domain/settings/settings_saver.h"
#include "domain/util/scheduler.h"
#include "generated/cq_protocol.h"

namespace cq {

// Composition-root state shared by app tasks. Members are added by later tasks; each module
// writes its own telemetry fields into `fast`/`slow`, the TX scheduler only stamps and sends.
struct App {
  // comms
  FrameParser parser;
  FrameWriter writer;
  EventQueue events;
  AckQueue acks;
  RejectLimiter rejects;
  LinkSession link;
  // telemetry staging
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
  bool fast_due = false;
  bool slow_due = false;
  proto::CalDataMsg cal_data{};
  bool cal_data_pending = false;
  proto::CalResultMsg cal_result{};
  bool cal_result_pending = false;
  proto::SettingValueMsg setting_value{};
  bool setting_value_pending = false;
  // health
  LoadMeter load;
  uint16_t scheduler_overruns = 0;
  proto::ResetReason reset_reason = proto::ResetReason::kUnknown;
  bool encoders_enabled = false;
  // settings
  Settings settings;
  EepromWriter eewriter;
  SettingsSaver saver;  // staged per block: the writer queue can never overflow (LLD §10.10)
  bool moving = false;  // set by the control task; saves are deferred while moving
};

extern App g_app;

}  // namespace cq
