#pragma once
#include <stdint.h>

#include "domain/arbitration/arbitrator.h"
#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"
#include "domain/estimation/battery_monitor.h"
#include "domain/input/nec_decoder.h"
#include "domain/lite/cam_lite.h"
#include "domain/lite/drive_mix_lite.h"
#include "domain/lite/encoder_lite.h"
#include "domain/lite/gyro_zero_lite.h"
#include "domain/lite/heading_hold_lite.h"
#include "domain/lite/range_lite.h"
#include "domain/lite/settings_lite.h"
#include "domain/lite/tilt_lite.h"
#include "domain/lite/yaw_lite.h"
#include "domain/safety/envelope.h"
#include "domain/safety/safety_supervisor.h"
#include "domain/selftest/self_test.h"
#include "domain/settings/eeprom_writer.h"
#include "domain/ui/annunciator.h"
#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"
#include "domain/util/motion_timer.h"
#include "generated/cq_protocol.h"
#include "types/hw_types.h"

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
  SettingsLite settings;
  bool settings_loaded = false;
  EepromWriter eewriter;
  bool save_requested = false;
  GyroZeroLite gyro_zero;
  YawLite yaw;
  TiltLite tilt;
  RangeLite range;
  BatteryMonitor batt;
  EncoderLite enc_l;
  EncoderLite enc_r;
  bool imu_ok = false;
  uint8_t imu_fail_streak = 0;
  NecDecoder nec;
  IrEvent ir_event{};
  bool ir_pending = false;
  Envelope envelope;
  Arbitrator arbitrator;
  SafetySupervisor supervisor;
  SafetyStatus safety{};
  SelfTest self_test;
  bool self_test_pending = true;
  HeadingHoldLite heading;
  RampLite ramp_l;
  RampLite ramp_r;
  MotionTimer motion;
  bool moving = false;
  int8_t teleop_v = 0;
  int8_t teleop_w = 0;
  bool deadman_latched = false;
  bool soft_stop = false;
  proto::OwnerHintValue owner_hint = proto::OwnerHintValue::kNone;
  CamLite cam;
  LedSequencer led;
  Annunciator annunciator;  // full firmware LED + sound rules (spec 02 §A9, §D4)
  SoundSequencer sound;
  uint8_t toggle_seq = 0;  // +1 per IR mute toggle → confirm blinks
  bool toggle_on = false;
  // LITE-STATE-END
};
extern LiteApp g_lite;

void setup();
void loop();
void commsPoll();
void txPump();
void telemetryFastDue();
void telemetrySlowDue();
void applySettings();

}  // namespace lite
}  // namespace cq
