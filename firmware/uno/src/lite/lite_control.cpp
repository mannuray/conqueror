#include "domain/input/ir_controller.h"
#include "domain/lite/drive_mix_lite.h"
#include "domain/lite/encoder_lite.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/clock.h"
#include "ports/motor.h"

namespace cq {
namespace lite {

namespace {
constexpr uint16_t kDeadmanMs = 300;      // full default (setting 15)
constexpr uint16_t kIrHandbackMs = 3000;  // full default (setting 19)
constexpr uint16_t kMaxCps = 2500;        // full default (settings 48/49)
IrController g_ir;

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_lite.fast_flags = on ? static_cast<uint16_t>(g_lite.fast_flags | m) : static_cast<uint16_t>(g_lite.fast_flags & ~m);
}
void zeroTeleop() {
  g_lite.teleop_v = 0;
  g_lite.teleop_w = 0;
}

void irInput(uint32_t now) {
  IrOutput o{};
  bool have = false;
  if (g_lite.ir_pending) {
    g_lite.ir_pending = false;
    if (g_ir.onEvent(g_lite.ir_event, o)) {
      if (g_lite.arbitrator.onIrInput(now)) zeroTeleop();  // IR takeover: network command dropped first
      have = true;
    }
  }
  if (!have && g_ir.tick(now, o)) have = true;
  if (!have) return;
  if (o.active) {
    g_lite.teleop_v = o.v;
    g_lite.teleop_w = o.w;
  } else {
    zeroTeleop();
  }
  if (o.jog_pan != 0 || o.jog_tilt != 0) g_lite.cam.nudge(static_cast<int16_t>(-o.jog_pan * 12), static_cast<int16_t>(o.jog_tilt * 12));
  if (o.stop) {
    zeroTeleop();
    g_lite.arbitrator.releaseIr();
  }
  if (o.cam_home) g_lite.cam.home();
  if (o.speed_changed) g_lite.beeper.beep(static_cast<uint16_t>(400 + g_ir.speedPct() * 10), 60, now);
}
}  // namespace

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const LiteSettings& s = g_lite.settings.values();

  // 1. ownership, deadman and link loss (reused Arbitrator: same rules as the full firmware)
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
  irInput(now);

  // 2. heading hold → mix → caps → ramp → PWM (whole numbers only)
  const int8_t v = g_lite.teleop_v;
  const int8_t steer = (s.reverse_steer == 0 && v < 0) ? static_cast<int8_t>(-g_lite.teleop_w) : g_lite.teleop_w;  // car-style
  int8_t w_steer = 0, w_ctrl = 0;
  g_lite.heading.teleop(v, steer, imuUsable(), g_lite.yaw.yawCdeg(), g_lite.yaw.rateDdps(), w_steer, w_ctrl);
  Tracks t = mixLite(v, w_steer, w_ctrl, s.turn_full);
  const uint8_t degraded = !imuUsable() ? 30 : (g_lite.range.fault() ? 50 : 100);
  const uint8_t cap = g_lite.envelope.speedCap() < degraded ? g_lite.envelope.speedCap() : degraded;
  bool capped = false;
  t = capLite(t, cap, g_lite.range.distMm(), s.obstacle_stop, s.obstacle_slow, g_lite.envelope.brakeOn(), capped);
  const uint16_t accel_env = g_lite.envelope.accelCap();
  const uint16_t accel = (accel_env != 0 && accel_env < s.accel_limit) ? accel_env : s.accel_limit;
  const int16_t pm_l = g_lite.ramp_l.step(t.l, accel);
  const int16_t pm_r = g_lite.ramp_r.step(t.r, accel);
  if (g_lite.soft_stop && pm_l == 0 && pm_r == 0) g_lite.soft_stop = false;
  const bool moving_l = g_lite.encoders_enabled ? g_lite.enc_l.moving() : g_lite.moving;
  const bool moving_r = g_lite.encoders_enabled ? g_lite.enc_r.moving() : g_lite.moving;
  const int16_t pwm_l = toPwmLite(pm_l, moving_l, s.db_start[pm_l >= 0 ? 0 : 1], s.db_keep[pm_l >= 0 ? 0 : 1]);
  const int16_t pwm_r = toPwmLite(pm_r, moving_r, s.db_start[pm_r >= 0 ? 2 : 3], s.db_keep[pm_r >= 0 ? 2 : 3]);

  // 3. the safety gate: only the supervisor makes PwmOutput (hard rule)
  const SafetyInputs in{g_lite.tilt.lifted(), g_lite.tilt.tilted(),
                        g_lite.batt.level() == BatteryMonitor::Level::kCritical, g_lite.envelope.brakeOn(),
                        g_lite.range.distMm(), s.obstacle_stop, 0, g_lite.self_test.locked()};
  const bool was_blocked = g_lite.safety.blocked;
  const PwmOutput out = g_lite.supervisor.apply(pwm_l, pwm_r, in, g_lite.safety);
  port::motor::apply(out);
  if (g_lite.safety.blocked && !was_blocked) g_lite.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_lite.range.distMm()));
  if (!g_lite.safety.blocked && was_blocked) g_lite.events.push(proto::EventKind::kUnblocked);

  // 4. state + telemetry
  if (out.left() != 0 || out.right() != 0) g_lite.motion.mark(now);
  g_lite.moving = g_lite.motion.moving(now);
  setFast(proto::FastFlagBit::kBlocked, g_lite.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_lite.safety.halted);
  setFast(proto::FastFlagBit::kCapped, capped);
  setFast(proto::FastFlagBit::kSoftStop, g_lite.soft_stop);
  setFast(proto::FastFlagBit::kLocked, g_lite.envelope.lockBrake());
  setFast(proto::FastFlagBit::kSynced, g_lite.link.synced());
  proto::TelemFastMsg& f = g_lite.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(pm_l / 10);
  f.spd_cmd_r = static_cast<int8_t>(pm_r / 10);
  f.spd_act_l = speedPctLite(g_lite.enc_l.cps(), out.left(), kMaxCps, g_lite.encoders_enabled);
  f.spd_act_r = speedPctLite(g_lite.enc_r.cps(), out.right(), kMaxCps, g_lite.encoders_enabled);
  f.pan = g_lite.cam.outPan();
  f.tilt = g_lite.cam.outTilt();
  f.flags = g_lite.fast_flags;
  g_lite.slow.owner = static_cast<uint8_t>(g_lite.arbitrator.owner());
  g_lite.slow.driving = g_lite.moving ? 1 : 0;
}

}  // namespace lite
}  // namespace cq
