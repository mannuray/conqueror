#include "app/app.h"
#include "app/motion_api.h"
#include "app/tasks.h"
#include "domain/motion/battery_comp.h"
#include "domain/motion/mixer.h"
#include "domain/motion/target_limiter.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/encoders.h"
#include "ports/motor.h"

namespace cq {
namespace app {
namespace {

constexpr float kDt = 0.01f;

PrimitiveConfig g_prim{180, 150, 329.0f, 7.6f, false, false};
SpeedConfig g_cfg[4] = {};  // l_fwd, l_rev, r_fwd, r_rev
bool g_soft_stop = false;

// Same 3-line helper as in imu_task/sensor_tasks: accepted duplication (file-local, no shared state).
void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

void zeroTeleop() {
  g_app.teleop_v = 0;
  g_app.teleop_w = 0;
  g_app.teleop_rate_mode = false;
  g_app.teleop_rate_dps = 0.0f;
}

void rejectEvent(uint8_t frame_type, uint8_t src, proto::AckStatus st) {
  if (g_app.rejects.allow(src, port::clock::nowMs())) {
    g_app.events.push(proto::EventKind::kRejected, frame_type, static_cast<int16_t>(st));
  }
}

bool imuUsable() { return g_app.imu_ok && g_app.imu_fail_streak < 10; }

PrimitiveInputs primInputs() {
  return PrimitiveInputs{g_app.yaw.yawDeg(), g_app.yaw.totalDeg(), g_app.yaw_rate_dps, g_app.enc_l.count(),
                         g_app.enc_r.count(), g_app.enc_health.stalled(0) || g_app.enc_health.stalled(1)};
}

void pushPrimitiveResult() {
  PrimitiveResult r{};
  if (!g_app.primitives.takeResult(r)) return;
  g_app.events.push(proto::EventKind::kPrimitiveDone,
                    static_cast<int16_t>(static_cast<uint8_t>(r.kind) | ((r.ok ? 1 : 0) << 8)),
                    static_cast<int16_t>(r.reason));
}

proto::AckStatus networkMotion(uint8_t frame_type, uint8_t src, bool teleop) {
  if (g_app.self_test.locked()) {  // LLD §10.9: critical self-test → every motion command refused
    rejectEvent(frame_type, src, proto::AckStatus::kBlocked);
    return proto::AckStatus::kBlocked;
  }
  const proto::AckStatus st = g_app.arbitrator.requestNetwork(teleop, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) rejectEvent(frame_type, src, st);
  return st;
}

// Accepted network motion: AI motion ends Override, human driving refreshes its idle timer (LLD §10.2).
void overrideMotion(uint8_t src, bool driving) {
  g_app.safety_override.onNetworkMotion(static_cast<proto::Source>(src), driving, port::clock::nowMs());
  drainOverride();
}

}  // namespace

void stopTeleop() { zeroTeleop(); }

SafetyOverride::Gate overrideGate() {
  const bool fault = g_app.lift.lifted() || g_app.lift.tilted() ||
                     g_app.batt.level() == BatteryMonitor::Level::kCritical ||
                     g_app.self_test.locked();  // LLD §10.9: Override can never lift the self-test lock
  return SafetyOverride::Gate{g_app.link.synced(), g_app.envelope.childPlay(), fault, g_app.arbitrator.owner()};
}

void drainOverride() {
  bool on = false;
  proto::OverrideReason why = proto::OverrideReason::kNetwork;
  while (g_app.safety_override.takeEvent(on, why)) {
    g_app.events.push(proto::EventKind::kSafetyOverride, on ? 1 : 0, static_cast<int16_t>(why));
    ++g_app.override_seq;
  }
  const uint32_t m = 1ul << static_cast<uint8_t>(proto::SlowFlagBit::kSafetyOverride);
  g_app.slow_flags = g_app.safety_override.active() ? (g_app.slow_flags | m) : (g_app.slow_flags & ~m);
}

void configureMotion() {
  const GlobalSettings& gs = g_app.settings.global();
  const ProfileSettings& ps = g_app.settings.profile();
  const PidGains speed = gainsFromMilli(ps.pid_speed_kp, ps.pid_speed_ki, ps.pid_speed_kd);
  const bool enc = g_app.encoders_enabled;
  g_cfg[0] = SpeedConfig{ps.deadband_start[0], ps.deadband_keep[0], ps.max_cps_l, speed, enc};
  g_cfg[1] = SpeedConfig{ps.deadband_start[1], ps.deadband_keep[1], ps.max_cps_l, speed, enc};
  g_cfg[2] = SpeedConfig{ps.deadband_start[2], ps.deadband_keep[2], ps.max_cps_r, speed, enc};
  g_cfg[3] = SpeedConfig{ps.deadband_start[3], ps.deadband_keep[3], ps.max_cps_r, speed, enc};
  g_app.heading.configure(HeadingConfig{gainsFromMilli(ps.pid_rate_kp, ps.pid_rate_ki, ps.pid_rate_kd),
                                        gainsFromMilli(ps.pid_heading_kp, ps.pid_heading_ki, ps.pid_heading_kd),
                                        gs.turn_rate_max});
  const float counts_per_mm = gs.counts_per_cm / 1000.0f;  // setting is counts/cm × 100
  const float v_full = (ps.max_cps_l + ps.max_cps_r) * 0.5f / counts_per_mm;
  g_prim = PrimitiveConfig{gs.turn_rate_max, ps.track_width_eff, v_full, counts_per_mm, enc, imuUsable()};
}

void controlInit() {
  port::motor::begin();
  port::motor::apply(PwmOutput::zero());
  port::encoders::begin();
}

proto::AckStatus onDrive(const uint8_t* p, uint8_t len) {
  proto::DriveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = networkMotion(static_cast<uint8_t>(proto::FrameType::kDrive), m.src, true);
  if (st != proto::AckStatus::kOk) return st;
  if (g_app.primitives.active() != proto::PrimitiveKind::kNone) g_app.primitives.cancel(proto::AckStatus::kCancelled);
  g_app.deadman_latched = false;
  g_app.teleop_rate_mode = false;
  g_app.teleop_v = m.v;
  g_app.teleop_w = m.w;
  overrideMotion(m.src, m.v != 0 || m.w != 0);
  return st;
}

proto::AckStatus onDriveRate(const uint8_t* p, uint8_t len) {
  proto::DriveRateMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (!imuUsable()) return proto::AckStatus::kUnavailable;
  const proto::AckStatus st = networkMotion(static_cast<uint8_t>(proto::FrameType::kDriveRate), m.src, true);
  if (st != proto::AckStatus::kOk) return st;
  if (g_app.primitives.active() != proto::PrimitiveKind::kNone) g_app.primitives.cancel(proto::AckStatus::kCancelled);
  g_app.deadman_latched = false;
  g_app.teleop_rate_mode = true;
  g_app.teleop_v = m.v;
  g_app.teleop_rate_dps = clampf(m.yaw_rate * 0.1f, -static_cast<float>(g_prim.turn_rate_max_dps),
                                 static_cast<float>(g_prim.turn_rate_max_dps));
  overrideMotion(m.src, m.v != 0 || m.yaw_rate != 0);
  return st;
}

proto::AckStatus onStop(const uint8_t* p, uint8_t len) {
  proto::StopMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  zeroTeleop();
  g_app.primitives.cancel(proto::AckStatus::kCancelled);
  calibrationAbort(proto::AckStatus::kCalCancelled);
  g_app.arbitrator.onNetworkStop();
  g_app.safety_override.end(proto::OverrideReason::kStop);  // any STOP ends Override
  drainOverride();
  // Accepted from any source. While IR owns, it only zeroes the current command; IR keeps ownership.
  return proto::AckStatus::kOk;
}

proto::AckStatus onTurn(const uint8_t* p, uint8_t len) {
  proto::TurnMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus own = networkMotion(static_cast<uint8_t>(proto::FrameType::kTurn), m.src, false);
  if (own != proto::AckStatus::kOk) return own;
  zeroTeleop();
  overrideMotion(m.src, true);
  g_prim.imu_ok = imuUsable();
  return g_app.primitives.startTurn(m, g_prim, primInputs(), port::clock::nowMs());
}

proto::AckStatus startLocalTurn(int16_t angle_dd, uint8_t speed_pct) {
  proto::TurnMsg m{};
  m.src = static_cast<uint8_t>(proto::Source::kNone);
  m.mode = static_cast<uint8_t>(proto::TurnMode::kBy);
  m.angle = angle_dd;
  m.speed = speed_pct;
  m.radius_cm = 0;
  zeroTeleop();
  g_prim.imu_ok = imuUsable();
  return g_app.primitives.startTurn(m, g_prim, primInputs(), port::clock::nowMs());
}

proto::AckStatus onMove(const uint8_t* p, uint8_t len) {
  proto::MoveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus own = networkMotion(static_cast<uint8_t>(proto::FrameType::kMove), m.src, false);
  if (own != proto::AckStatus::kOk) return own;
  zeroTeleop();
  overrideMotion(m.src, true);
  g_prim.imu_ok = imuUsable();  // MOVE holds a gyro heading: UNAVAILABLE without the IMU (R7)
  g_prim.encoders = g_app.encoders_enabled && !g_app.enc_health.faulted(0) && !g_app.enc_health.faulted(1);
  return g_app.primitives.startMove(m, g_prim, primInputs(), port::clock::nowMs());
}

proto::AckStatus onCancel(const uint8_t* p, uint8_t len) {
  proto::CancelMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (g_app.arbitrator.owner() == proto::Owner::kIr) return proto::AckStatus::kNotOwner;
  g_app.primitives.cancel(proto::AckStatus::kCancelled);
  return proto::AckStatus::kOk;
}

proto::AckStatus onBrake(const uint8_t* p, uint8_t len) {
  proto::BrakeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  return g_app.envelope.setBrake(m.on != 0);
}

proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len) {
  proto::EnvelopeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.envelope.set(m.speed_cap, m.accel_cap, m.lock_brake);
  if (st == proto::AckStatus::kOk) {
    const uint16_t packed = static_cast<uint16_t>(static_cast<uint16_t>(m.speed_cap) |
                                                  static_cast<uint16_t>(static_cast<uint16_t>(m.accel_cap) << 8u));
    g_app.events.push(proto::EventKind::kEnvelopeChanged, static_cast<int16_t>(packed), g_app.envelope.flags());
    g_app.slow.env_speed_cap = m.speed_cap;
    g_app.slow.env_accel_cap = m.accel_cap;
    g_app.slow.env_lock_brake = g_app.envelope.flags();  // EnvelopeFlagBit bits (child play = 3)
    // A script runs without the target limiter, so an envelope arriving mid-script stops it (LLD §9.7).
    if (g_app.envelope.blocksCalibration()) calibrationAbort(proto::AckStatus::kEnvelopeLocked);
    if (g_app.envelope.childPlay()) {  // only child play ends Override; an adult envelope keeps it (caps apply)
      g_app.safety_override.end(proto::OverrideReason::kEnvelope);
      drainOverride();
    }
  }
  return st;
}

proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len) {
  proto::OwnerHintMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.owner_hint = static_cast<proto::OwnerHintValue>(m.owner);
  g_app.safety_override.onOwnerHint(g_app.owner_hint);  // handed to the AI → Override ends
  drainOverride();
  return proto::AckStatus::kOk;
}

proto::AckStatus onSafetyOverride(const uint8_t* p, uint8_t len) {
  proto::SafetyOverrideMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.safety_override.requestNetwork(m.on != 0, static_cast<proto::Source>(m.src),
                                                                   overrideGate(), port::clock::nowMs());
  if (st != proto::AckStatus::kOk) rejectEvent(static_cast<uint8_t>(proto::FrameType::kSafetyOverride), m.src, st);
  drainOverride();
  return st;
}

proto::AckStatus onProfile(const uint8_t* p, uint8_t len) {
  proto::ProfileMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  // MVP: a single surface profile (index 0).
  if (m.op == static_cast<uint8_t>(proto::ProfileOp::kSelect) && m.index == 0) return proto::AckStatus::kOk;
  return proto::AckStatus::kUnavailable;
}

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const GlobalSettings& gs = g_app.settings.global();
  encodersTick();

  // 1. ownership and link/deadman rules. Network motion is BUSY until HELLO_ACK (LLD §9.4); the
  //    Network → None idle release waits while a primitive runs (LLD §9.18).
  g_app.arbitrator.setSynced(g_app.link.synced());
  const bool prim_busy = g_app.primitives.active() != proto::PrimitiveKind::kNone;
  switch (g_app.arbitrator.tick(now, g_app.link.alive(now), prim_busy, Arbitrator::Config{gs.deadman, gs.ir_handback})) {
    case Arbitrator::Action::kDeadmanStop:
      zeroTeleop();
      g_soft_stop = true;
      g_app.deadman_latched = true;
      g_app.events.push(proto::EventKind::kDeadmanStop);
      break;
    case Arbitrator::Action::kLinkLostStop:
      calibrationAbort(proto::AckStatus::kLinkLost);
      zeroTeleop();
      g_soft_stop = true;
      g_app.primitives.cancel(proto::AckStatus::kLinkLost);  // never resumed
      g_app.safety_override.onLinkLost();
      break;
    case Arbitrator::Action::kIrReleased:
      zeroTeleop();
      break;
    case Arbitrator::Action::kNone:
      break;
  }
  if (g_app.arbitrator.takeOwnerChanged()) {
    const proto::Owner owner = g_app.arbitrator.owner();
    g_app.events.push(proto::EventKind::kControlChanged, static_cast<int16_t>(owner));
    // IR takeover: Task 14's input task cancels primitives and stops teleop *before* the IR command is
    // applied (doing it here would cancel a TURN the IR `0` key just started).
    if (owner == proto::Owner::kIr) g_app.events.push(proto::EventKind::kIrOverride);
    // An idle release, STOP or calibration never resumes an old DRIVE/DRIVE_RATE (IR writes teleop while it owns).
    if (owner == proto::Owner::kNone || owner == proto::Owner::kCalibration) zeroTeleop();
    g_app.safety_override.onOwner(owner);  // another principal took over → Override ends
  }
  g_app.safety_override.tick(now, SafetyOverride::Config{gs.override_idle_s, gs.override_max_s});
  drainOverride();
  // Effective obstacle brake: off while Override is on (LLD §10.4), for the slow zone and the stop alike.
  const bool brake_on = g_app.safety_override.brakeOn(g_app.envelope.brakeOn());

  // 2. motion source
  int16_t pwm_l = 0;
  int16_t pwm_r = 0;
  const bool calibrating = calibrationTick(pwm_l, pwm_r);
  bool capped = false;
  float w = 0.0f;
  float tgt_l = 0.0f;
  float tgt_r = 0.0f;
  if (calibrating) {
    g_app.ramp_l.reset();
    g_app.ramp_r.reset();
    g_app.speed_l.reset();
    g_app.speed_r.reset();
    g_app.heading.reset();
  } else {
    const ProfileSettings& ps = g_app.settings.profile();
    const float yaw = g_app.yaw.yawDeg();
    const float rate = g_app.yaw_rate_dps;
    // A TURN cannot finish without the gyro (R7): end it; a running MOVE goes on without heading hold.
    if (!imuUsable() && g_app.primitives.active() == proto::PrimitiveKind::kTurn) {
      g_app.primitives.cancel(proto::AckStatus::kUnavailable);
    }
    float v = 0.0f;
    MotionSetpoint sp{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
    if (g_app.primitives.tick(primInputs(), kDt, now, sp)) {
      v = sp.v_pct;
      w = imuUsable() ? g_app.heading.update(sp.mode, 0.0f, sp.rate_dps, sp.heading_deg, yaw, rate, kDt, false)
                      : 0.0f;
    } else if (g_app.teleop_rate_mode) {
      v = g_app.teleop_v;
      w = imuUsable() ? g_app.heading.update(HeadingMode::kRate, 0.0f, g_app.teleop_rate_dps, 0.0f, yaw, rate, kDt, false)
                      : g_app.teleop_rate_dps * 100.0f / gs.turn_rate_max;
    } else {
      v = g_app.teleop_v;
      w = imuUsable() ? g_app.heading.teleop(v, g_app.teleop_w, yaw, rate, kDt, false)
                      : static_cast<float>(g_app.teleop_w);
    }

    // 3. mix (turning priority, pivot fallback), envelope/degraded caps and obstacle slow in %, ramps
    const float keep_pct = (ps.deadband_keep[0] + ps.deadband_keep[1] + ps.deadband_keep[2] + ps.deadband_keep[3]) *
                           100.0f / (4.0f * 255.0f);
    float l = 0.0f, r = 0.0f;
    mix(v, w, MixerConfig{ps.turn_rate_full_speed, gs.reverse_steer == 0, gs.pivot_fallback != 0, keep_pct}, l, r);
    const uint8_t degraded = !imuUsable() ? 30 : (g_app.us.fault() ? 50 : 100);  // LLD §9.5/§9.16 (min of both)
    const Limited lim = limitTargets(l, r, Limits{g_app.envelope.speedCap(), g_app.us.distMm(), gs.obstacle_stop,
                                                  gs.obstacle_slow, brake_on, degraded});
    capped = lim.capped;
    // The envelope accel cap may only tighten the global limit, never loosen it.
    const float accel_lim = static_cast<float>(gs.accel_limit);
    const float accel_env = static_cast<float>(g_app.envelope.accelCap());
    const float accel = (accel_env != 0.0f && accel_env < accel_lim) ? accel_env : accel_lim;
    tgt_l = g_app.ramp_l.step(lim.l, kDt, accel, accel * 2.0f);
    tgt_r = g_app.ramp_r.step(lim.r, kDt, accel, accel * 2.0f);
    if (g_soft_stop && absf(tgt_l) < 0.5f && absf(tgt_r) < 0.5f) g_soft_stop = false;

    // 4. per-track speed control (closed loop only with a healthy encoder). The integrator is frozen while
    //    the target is capped (R4) or the supervisor cuts a stalled track (no wind-up → no lurch after 1 s).
    SpeedConfig cl = g_cfg[tgt_l >= 0.0f ? 0 : 1];
    SpeedConfig cr = g_cfg[tgt_r >= 0.0f ? 2 : 3];
    cl.closed_loop = cl.closed_loop && !g_app.enc_health.faulted(0);
    cr.closed_loop = cr.closed_loop && !g_app.enc_health.faulted(1);
    g_app.speed_l.configure(cl);
    g_app.speed_r.configure(cr);
    const bool moving_l = g_app.encoders_enabled ? g_app.enc_l.recent() : g_app.moving;
    const bool moving_r = g_app.encoders_enabled ? g_app.enc_r.recent() : g_app.moving;
    const uint8_t cap = g_app.envelope.speedCap();
    pwm_l = g_app.speed_l.update(tgt_l, g_app.enc_l.cps(), kDt, pwmLimitForCap(cap, cl.keep_pwm), moving_l,
                                 capped || g_app.enc_health.stalled(0));
    pwm_r = g_app.speed_r.update(tgt_r, g_app.enc_r.cps(), kDt, pwmLimitForCap(cap, cr.keep_pwm), moving_r,
                                 capped || g_app.enc_health.stalled(1));

  }

  // 5. never more than motor_v_max on the 6 V motors — every source, calibration scripts included
  const float s = batteryScale(g_app.batt.mv(), gs.motor_v_max);
  pwm_l = scalePwm(pwm_l, s);
  pwm_r = scalePwm(pwm_r, s);

  // 6. safety gate → motors (req_pwm_* = asked of the supervisor, used by EncoderHealth to clear a stall)
  g_app.req_pwm_l = pwm_l;
  g_app.req_pwm_r = pwm_r;
  // Override only reaches the supervisor through brake_on; lift, battery critical and stall never consult it.
  const SafetyInputs in{g_app.lift.lifted(), g_app.lift.tilted(),
                        g_app.batt.level() == BatteryMonitor::Level::kCritical, brake_on,
                        g_app.us.distMm(), gs.obstacle_stop, g_app.enc_health.stalledMask(),
                        g_app.self_test.locked()};
  const bool was_blocked = g_app.safety.blocked;
  const PwmOutput out = g_app.supervisor.apply(pwm_l, pwm_r, in, g_app.safety);
  port::motor::apply(out);

  if ((g_app.safety.blocked || g_app.safety.halted || g_app.safety.coast) &&
      g_app.primitives.active() != proto::PrimitiveKind::kNone) {
    g_app.primitives.cancel(proto::AckStatus::kBlocked);
  }
  pushPrimitiveResult();
  if (g_app.safety.blocked && !was_blocked) g_app.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_app.us.distMm()));
  if (!g_app.safety.blocked && was_blocked) g_app.events.push(proto::EventKind::kUnblocked);

  // 7. state + telemetry
  g_app.out_pwm_l = out.left();
  g_app.out_pwm_r = out.right();
  if (out.left() != 0 || out.right() != 0) g_app.last_motion_ms = now;
  g_app.moving = !elapsed(now, g_app.last_motion_ms, 300);
  g_app.stationary = !g_app.moving;

  setFast(proto::FastFlagBit::kBlocked, g_app.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_app.safety.halted);
  setFast(proto::FastFlagBit::kCapped, capped || g_app.speed_l.saturated() || g_app.speed_r.saturated());
  setFast(proto::FastFlagBit::kTurnLimited, absf(w) >= 99.9f);
  setFast(proto::FastFlagBit::kSoftStop, g_soft_stop);
  setFast(proto::FastFlagBit::kLocked, g_app.envelope.lockBrake());
  setFast(proto::FastFlagBit::kCalibrating, calibrating);
  setFast(proto::FastFlagBit::kStallL, g_app.enc_health.stalled(0));
  setFast(proto::FastFlagBit::kStallR, g_app.enc_health.stalled(1));
  setFast(proto::FastFlagBit::kEncoderFault, g_app.enc_health.faulted(0) || g_app.enc_health.faulted(1));
  setFast(proto::FastFlagBit::kSynced, g_app.link.synced());

  proto::TelemFastMsg& f = g_app.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(tgt_l);
  f.spd_cmd_r = static_cast<int8_t>(tgt_r);
  f.flags = g_app.fast_flags;
  f.prim = static_cast<uint8_t>(g_app.primitives.active());
  f.prim_pct = g_app.primitives.progressPct();
  g_app.slow.owner = static_cast<uint8_t>(g_app.arbitrator.owner());
  g_app.slow.driving = g_app.moving ? 1 : 0;
}

}  // namespace app
}  // namespace cq
