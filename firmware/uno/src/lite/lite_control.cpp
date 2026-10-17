#include "domain/input/ir_controller.h"
#include "domain/lite/drive_mix_lite.h"
#include "domain/lite/encoder_lite.h"
#include "domain/ui/catalogues.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/clock.h"
#include "ports/motor.h"

namespace cq {
namespace lite {

namespace {
constexpr uint16_t kDeadmanMs = 300;      // full default (setting 15)
constexpr uint16_t kIrHandbackMs = 3000;  // full default (setting 19)
constexpr uint16_t kMaxCps = StallGuardLite::kMaxCps;  // 2500 counts/s at PWM 255 (design-notes §70)
IrController g_ir;
bool g_ir_driving = false;  // an IR drive key is held (its release frees the stall / runaway latch)

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_lite.fast_flags = on ? static_cast<uint16_t>(g_lite.fast_flags | m) : static_cast<uint16_t>(g_lite.fast_flags & ~m);
}
void zeroTeleop() {
  g_lite.teleop_v = 0;
  g_lite.teleop_w = 0;
}

// IR key 9 (as the full input task): battery level as an LED colour for 2 s + 3/2/1 beeps.
void batteryCheck(uint32_t now) {
  Rgb c{40, 40, 40};
  uint8_t count = 0;
  switch (g_lite.batt.level()) {
    case BatteryMonitor::Level::kOk: c = Rgb{0, 150, 0}; count = 3; break;
    case BatteryMonitor::Level::kLow: c = Rgb{160, 80, 0}; count = 2; break;
    case BatteryMonitor::Level::kCritical: c = Rgb{255, 0, 0}; count = 1; break;
    case BatteryMonitor::Level::kUsb: break;  // no pack: white, no beeps
  }
  g_lite.led.set(LedLayer::kAttention, {proto::LedMode::kSolid, c, 0, 2000}, now);
  Note n[SoundSequencer::kMaxNotes];
  uint8_t k = 0;
  for (uint8_t i = 0; i < count && k + 1 < SoundSequencer::kMaxNotes; ++i) {
    n[k++] = Note{1500, 80};
    n[k++] = Note{0, 120};
  }
  if (k > 0) g_lite.sound.play(n, k, SoundSequencer::kPrioEvent, now);
}

// Sounds and one-shot keys of an IR output (full input_task.cpp; Lite: no Override, parking beeps or TURN 180).
void irExtras(const IrOutput& o, uint32_t now) {
  if (o.cam_home) g_lite.cam.home();
  if (o.speed_changed) {
    const Note n{static_cast<uint16_t>(400 + g_ir.speedPct() * 10), 60};
    g_lite.sound.play(&n, 1, SoundSequencer::kPrioEvent, now);
  }
  if (o.toggle_mute) {
    const bool muted = g_lite.sound.mode() == proto::AudioModeValue::kMute;
    g_lite.sound.setMode(muted ? proto::AudioModeValue::kNormal : proto::AudioModeValue::kMute);
    if (muted) playSound(proto::SoundId::kBlip, SoundSequencer::kPrioEvent, now);
    ++g_lite.toggle_seq;  // 3 blinks: green = mute on, red = off
    g_lite.toggle_on = !muted;
  }
  if (o.battery_check) batteryCheck(now);
}

void irInput(uint32_t now) {
  IrOutput o{};
  bool have = false;
  if (g_lite.ir_pending) {
    g_lite.ir_pending = false;
    if (g_ir.onEvent(g_lite.ir_event, o)) {
      if (g_lite.arbitrator.onIrInput(now)) zeroTeleop();  // IR takeover: network command dropped first
      // Owner addition (spec 02 §D4): a click on each new key press, not on NEC repeats while held.
      if (!g_lite.ir_event.repeat) playSound(proto::SoundId::kTick, SoundSequencer::kPrioInfo, now);
      have = true;
    }
  }
  if (!have && g_ir.tick(now, o)) have = true;
  if (!have) return;
  if (o.active) {
    g_lite.teleop_v = o.v;
    g_lite.teleop_w = o.w;
    g_lite.teleop_auto = false;  // IR is a driver's input: car-style steering applies
    g_ir_driving = true;
  } else {
    zeroTeleop();
    if (g_ir_driving) g_lite.latch.onDriverRelease();  // drive key let go (spec 02 §F4; not a camera key)
    g_ir_driving = false;
  }
  g_lite.cam.jog(o.jog_pan, o.jog_tilt);  // held: applied every 20 ms in cameraTask (60 °/s); release → 0
  if (o.stop) {
    zeroTeleop();
    g_lite.latch.clear();  // IR OK = STOP
    g_lite.arbitrator.releaseIr();
  }
  irExtras(o, now);
}
// 1. ownership, deadman and link loss (reused Arbitrator: same rules as the full firmware)
void ownershipStep(uint32_t now) {
  g_lite.arbitrator.setSynced(g_lite.link.synced());
  switch (g_lite.arbitrator.tick(now, g_lite.link.alive(now), false, Arbitrator::Config{kDeadmanMs, kIrHandbackMs})) {
    case Arbitrator::Action::kDeadmanStop:
      zeroTeleop();
      g_lite.soft_stop = true;
      g_lite.deadman_latched = true;
      g_lite.events.push(proto::EventKind::kDeadmanStop);
      break;
    case Arbitrator::Action::kLinkLostStop:
      zeroTeleop();
      g_lite.soft_stop = true;
      break;
    case Arbitrator::Action::kIrReleased: zeroTeleop(); break;
    case Arbitrator::Action::kNone: break;
  }
  if (g_lite.arbitrator.takeOwnerChanged()) {
    const proto::Owner owner = g_lite.arbitrator.owner();
    g_lite.events.push(proto::EventKind::kControlChanged, static_cast<int16_t>(owner));
    if (owner == proto::Owner::kIr) g_lite.events.push(proto::EventKind::kIrOverride);
    if (owner == proto::Owner::kNone) zeroTeleop();
  }
}

struct DriveOut {
  int16_t pm_l;  // ramped track targets, 0.1 %
  int16_t pm_r;
  int16_t pwm_l;  // after deadband: asked of the supervisor
  int16_t pwm_r;
  bool capped;
};

// 2. heading hold → mix → caps → ramp → PWM (whole numbers only)
DriveOut driveTargets(const LiteSettings& s) {
  DriveOut d{};
  const int8_t v = g_lite.teleop_v;
  const int8_t steer = steerLite(v, g_lite.teleop_w, s.reverse_steer == 0, g_lite.teleop_auto);  // car-style
  int8_t w_steer = 0, w_ctrl = 0;
  g_lite.heading.freezeIntegral(g_lite.safety.blocked || g_lite.latch.latched());  // plan J review M2
  g_lite.heading.teleop(v, steer, imuUsable(), g_lite.yaw.yawCdeg(), g_lite.yaw.rateDdps(), w_steer, w_ctrl);
  if (g_lite.runaway.step(g_lite.heading.holding(), g_lite.heading.errorCdeg(), w_ctrl)) {  // spec 02 §F1.3
    g_lite.events.push(proto::EventKind::kHeadingFault, static_cast<int16_t>(g_lite.heading.errorCdeg() / 100));
    g_lite.latch.trip(MotionLatchLite::Reason::kHeading, -1);
  }
  Tracks t = liteMix(v, w_steer, w_ctrl, g_lite.teleop_auto, s.turn_full);  // §F1.4
  if (w_steer == 0 && !g_lite.teleop_auto) t = balanceTracks(t, s.track_balance);  // §F5.1 (straight only)
  const bool straight = g_lite.encoders_enabled && !g_lite.stall.faulted(0) && !g_lite.stall.faulted(1) &&
                        w_steer == 0 && t.l != 0 && t.r != 0 && ((t.l > 0) == (t.r > 0)) && !g_lite.latch.latched();
  t = g_lite.sync.apply(t, straight, g_lite.enc_l.count(), g_lite.enc_r.count(), g_lite.enc_l.cps(),
                        g_lite.enc_r.cps(), s.track_balance);  // §F1.1 + §F5.1: count ratio, gyro outside
  const uint8_t degraded_pct = !imuUsable() ? 30 : (g_lite.range.fault() ? 50 : 100);
  const uint8_t cap_pct = g_lite.envelope.speedCap() < degraded_pct ? g_lite.envelope.speedCap() : degraded_pct;
  t = capLite(t, cap_pct, g_lite.range.distMm(), s.obstacle_stop, s.obstacle_slow, g_lite.envelope.brakeOn(), d.capped);
  const uint16_t accel_env_pct_s = g_lite.envelope.accelCap();  // the envelope may only tighten the global limit
  const uint16_t accel_pct_s =
      (accel_env_pct_s != 0 && accel_env_pct_s < s.accel_limit) ? accel_env_pct_s : s.accel_limit;
  if (g_lite.latch.latched()) {  // floor test 2026-10-17: while both tracks are cut the ramps must not keep the old
    g_lite.ramp_l.reset();       // targets - a STOP / release would otherwise ramp DOWN from them and re-run the
    g_lite.ramp_r.reset();       // motors (seen: -249 / 249 for 0.3 s after a heading fault). Start from standstill.
    g_lite.heading.reset();
    t.l = t.r = 0;
  }
  d.pm_l = g_lite.ramp_l.step(t.l, accel_pct_s);
  d.pm_r = g_lite.ramp_r.step(t.r, accel_pct_s);
  if (g_lite.soft_stop && d.pm_l == 0 && d.pm_r == 0) g_lite.soft_stop = false;
  const bool moving_l = g_lite.encoders_enabled ? g_lite.enc_l.moving() : g_lite.moving;
  const bool moving_r = g_lite.encoders_enabled ? g_lite.enc_r.moving() : g_lite.moving;
  d.pwm_l = toPwmLite(d.pm_l, moving_l, s.db_start[d.pm_l >= 0 ? 0 : 1], s.db_keep[d.pm_l >= 0 ? 0 : 1]);
  d.pwm_r = toPwmLite(d.pm_r, moving_r, s.db_start[d.pm_r >= 0 ? 2 : 3], s.db_keep[d.pm_r >= 0 ? 2 : 3]);
  return d;
}

// 3. the safety gate: only the supervisor makes PwmOutput (hard rule)
PwmOutput safetyGate(const LiteSettings& s, const DriveOut& d) {
  const uint8_t stall_mask = g_lite.latch.stallMask();  // spec 02 §F1 / §F4: both tracks while latched
  const SafetyInputs in{g_lite.tilt.lifted(), g_lite.tilt.tilted(),
                        g_lite.batt.level() == BatteryMonitor::Level::kCritical, g_lite.envelope.brakeOn(),
                        g_lite.range.distMm(), s.obstacle_stop, stall_mask, g_lite.self_test.locked()};
  const bool was_blocked = g_lite.safety.blocked;
  const PwmOutput out = g_lite.supervisor.apply(d.pwm_l, d.pwm_r, in, g_lite.safety);
  if (g_lite.safety.blocked && !was_blocked) g_lite.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_lite.range.distMm()));
  if (!g_lite.safety.blocked && was_blocked) g_lite.events.push(proto::EventKind::kUnblocked);
  return out;
}

// 4. state + telemetry
void publish(uint32_t now, const PwmOutput& out, const DriveOut& d) {
  if (out.left() != 0 || out.right() != 0) g_lite.motion.mark(now);
  g_lite.moving = g_lite.motion.moving(now);
  setFast(proto::FastFlagBit::kBlocked, g_lite.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_lite.safety.halted);
  setFast(proto::FastFlagBit::kCapped, d.capped);
  setFast(proto::FastFlagBit::kSoftStop, g_lite.soft_stop);
  setFast(proto::FastFlagBit::kLocked, g_lite.envelope.lockBrake());
  setFast(proto::FastFlagBit::kSynced, g_lite.link.synced());
  const bool stalled = g_lite.latch.reason() == MotionLatchLite::Reason::kStall;
  setFast(proto::FastFlagBit::kStallL, stalled && g_lite.latch.side() == 0);
  setFast(proto::FastFlagBit::kStallR, stalled && g_lite.latch.side() == 1);
  setFast(proto::FastFlagBit::kEncoderFault, g_lite.stall.faulted(0) || g_lite.stall.faulted(1));
  proto::TelemFastMsg& f = g_lite.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(d.pm_l / 10);
  f.spd_cmd_r = static_cast<int8_t>(d.pm_r / 10);
  f.spd_act_l = speedPctLite(g_lite.enc_l.cps(), out.left(), kMaxCps, g_lite.encoders_enabled);
  f.spd_act_r = speedPctLite(g_lite.enc_r.cps(), out.right(), kMaxCps, g_lite.encoders_enabled);
  f.pan = g_lite.cam.outPan();
  f.tilt = g_lite.cam.outTilt();
  f.flags = g_lite.fast_flags;
  g_lite.slow.owner = static_cast<uint8_t>(g_lite.arbitrator.owner());
  g_lite.slow.driving = g_lite.moving ? 1 : 0;
}
}  // namespace

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const LiteSettings& s = g_lite.settings.values();
  ownershipStep(now);
  irInput(now);
  const DriveOut d = driveTargets(s);
  const PwmOutput out = safetyGate(s, d);
  port::motor::apply(out);
  publish(now, out, d);
}

}  // namespace lite
}  // namespace cq
