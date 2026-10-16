#pragma once
#include <stdint.h>

#include "domain/util/motion_timer.h"
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
#include "domain/estimation/gyro_zero_tracker.h"
#include "domain/estimation/lift_detector.h"
#include "domain/estimation/yaw_integrator.h"
#include "types/hw_types.h"
#include "domain/estimation/battery_monitor.h"
#include "domain/estimation/ultrasonic_filter.h"
#include "domain/input/nec_decoder.h"
#include "domain/arbitration/arbitrator.h"
#include "domain/safety/envelope.h"
#include "domain/safety/safety_override.h"
#include "domain/safety/safety_supervisor.h"
#include "domain/calibration/cal_script_runner.h"
#include "domain/estimation/encoder_health.h"
#include "domain/estimation/encoder_track.h"
#include "domain/selftest/self_test.h"
#include "domain/motion/heading_controller.h"
#include "domain/motion/primitives.h"
#include "domain/motion/ramp.h"
#include "domain/motion/speed_controller.h"
#include "domain/camera/camera_head.h"
#include "domain/ui/annunciator.h"
#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"

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
  GyroZeroTracker gyro_zero;  // learned gyro zero (LLD §11.8), seeded from gyro_bias_x/y/z
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
  // safety / arbitration
  Envelope envelope;
  Arbitrator arbitrator;
  SafetySupervisor supervisor;
  SafetyStatus safety{};
  SafetyOverride safety_override;  // LLD §10: never persistent, off at boot
  uint8_t override_seq = 0;        // +1 per Override enter/exit (UI blinks + chirp, Task 13)
  proto::OwnerHintValue owner_hint = proto::OwnerHintValue::kNone;
  int8_t teleop_v = 0;   // DRIVE, %
  int8_t teleop_w = 0;
  bool teleop_auto = false;  // last DRIVE had src AUTO (bridge motion service): no car-style flip
  bool teleop_rate_mode = false;  // DRIVE_RATE: closed-loop yaw rate instead of stick w
  float teleop_rate_dps = 0.0f;
  bool deadman_latched = false;   // DEADMAN_STOP fired; cleared by the next accepted DRIVE/DRIVE_RATE
  Primitives primitives;
  HeadingController heading;
  SpeedController speed_l;
  SpeedController speed_r;
  Ramp ramp_l;
  Ramp ramp_r;
  int16_t req_pwm_l = 0;  // asked of the supervisor (before the gate): a stalled track's direction
  int16_t req_pwm_r = 0;
  int16_t out_pwm_l = 0;  // applied by the supervisor
  int16_t out_pwm_r = 0;
  MotionTimer motion;
  // encoders / calibration / self-test
  EncoderTrack enc_l;
  EncoderTrack enc_r;
  EncoderHealth enc_health;
  CalScriptRunner cal;
  SelfTest self_test;
  uint8_t settings_reset_mask = 0;
  bool self_test_pending = true;     // a run is due at self_test_due_ms (boot run, or a SELF_TEST re-run)
  uint32_t self_test_due_ms = 2000;  // boot: 2 s after reset; re-run: +1500 ms (one 1 s IMU re-init try first)
  CameraHead camera;
  // ui
  SoundSequencer sound;
  LedSequencer led;
  Annunciator annunciator;
  Rgb led_out{1, 1, 1};  // forces the first write
  uint8_t bump_seq = 0;
  uint8_t toggle_seq = 0;  // +1 per IR toggle (mute, parking beeps) → 3 confirm blinks (Task 14)
  bool toggle_on = false;  // state after the last toggle: green = on, red = off
};

extern App g_app;

}  // namespace cq
