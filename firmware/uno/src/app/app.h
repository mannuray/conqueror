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
#include "domain/estimation/accel_variance.h"
#include "domain/estimation/attitude.h"
#include "domain/estimation/bump_detector.h"
#include "domain/estimation/lift_detector.h"
#include "domain/estimation/yaw_integrator.h"
#include "types/hw_types.h"
#include "domain/estimation/battery_monitor.h"
#include "domain/estimation/ultrasonic_filter.h"
#include "domain/input/nec_decoder.h"

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
  // imu
  Attitude attitude;
  YawIntegrator yaw;
  BumpDetector bump;
  LiftDetector lift;
  AccelVariance accel_var;
  ImuRaw imu_raw{};
  bool imu_ok = false;
  uint8_t imu_fail_streak = 0;
  float yaw_rate_dps = 0.0f;
  bool stationary = true;  // set by the control task: no PWM output for 300 ms
  uint16_t fast_flags = 0;
  uint32_t slow_flags = 0;
  // sensors
  UltrasonicFilter us;
  BatteryMonitor batt;
  NecDecoder nec;
  IrEvent ir_event{};
  bool ir_pending = false;  // consumed by the IR controller (Task 14)
};

extern App g_app;

}  // namespace cq
