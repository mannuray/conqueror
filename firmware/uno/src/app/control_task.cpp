#include "app/app.h"
#include "app/motion_api.h"
#include "app/tasks.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/motor.h"

namespace cq {
namespace app {
namespace {

// Same 3-line helper as in imu_task/sensor_tasks: accepted duplication (file-local, no shared state).
void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

void zeroTeleop() {
  g_app.teleop_v = 0;
  g_app.teleop_w = 0;
}

void rejectEvent(uint8_t frame_type, uint8_t src, proto::AckStatus st) {
  if (g_app.rejects.allow(src, port::clock::nowMs())) {
    g_app.events.push(proto::EventKind::kRejected, frame_type, static_cast<int16_t>(st));
  }
}

}  // namespace

SafetyOverride::Gate overrideGate() {
  // Task 7 adds the self-test lock to `fault`.
  const bool fault = g_app.lift.lifted() || g_app.lift.tilted() ||
                     g_app.batt.level() == BatteryMonitor::Level::kCritical;
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

void controlInit() {
  port::motor::begin();
  port::motor::apply(PwmOutput::zero());
}

proto::AckStatus onDrive(const uint8_t* p, uint8_t len) {
  proto::DriveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.arbitrator.requestNetwork(true, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) {
    rejectEvent(static_cast<uint8_t>(proto::FrameType::kDrive), m.src, st);
    return st;
  }
  g_app.teleop_v = m.v;
  g_app.teleop_w = m.w;
  g_app.safety_override.onNetworkMotion(static_cast<proto::Source>(m.src), m.v != 0 || m.w != 0,
                                        port::clock::nowMs());
  drainOverride();
  return st;
}

proto::AckStatus onStop(const uint8_t* p, uint8_t len) {
  proto::StopMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  zeroTeleop();
  g_app.arbitrator.onNetworkStop();
  g_app.safety_override.end(proto::OverrideReason::kStop);
  drainOverride();
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

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const GlobalSettings& gs = g_app.settings.global();
  const Arbitrator::Config cfg{gs.deadman, gs.ir_handback};
  // Network motion is BUSY until HELLO_ACK (LLD §9.4); a DRIVE in the same 10 ms as the HELLO_ACK may
  // still get BUSY — the next one of the stream is accepted.
  g_app.arbitrator.setSynced(g_app.link.synced());
  switch (g_app.arbitrator.tick(now, g_app.link.alive(now), false /* v1: no primitives yet */, cfg)) {
    case Arbitrator::Action::kDeadmanStop:
      zeroTeleop();
      g_app.events.push(proto::EventKind::kDeadmanStop);
      break;
    case Arbitrator::Action::kLinkLostStop:
      zeroTeleop();
      g_app.safety_override.onLinkLost();
      break;
    case Arbitrator::Action::kIrReleased:
      zeroTeleop();
      break;
    case Arbitrator::Action::kNone:
      break;
  }
  if (g_app.arbitrator.takeOwnerChanged()) {
    g_app.events.push(proto::EventKind::kControlChanged, static_cast<int16_t>(g_app.arbitrator.owner()));
    g_app.safety_override.onOwner(g_app.arbitrator.owner());
  }
  g_app.safety_override.tick(now, SafetyOverride::Config{gs.override_idle_s, gs.override_max_s});
  drainOverride();

  // Only a network owner's DRIVE values count: an idle release, IR or calibration never resumes an old DRIVE.
  if (g_app.arbitrator.owner() != proto::Owner::kNetwork) zeroTeleop();

  // v1 open loop: arcade mix, envelope cap in %, linear % → PWM.
  const float cap = g_app.envelope.speedCap();
  float l = clampf(static_cast<float>(g_app.teleop_v) - g_app.teleop_w, -100.0f, 100.0f);
  float r = clampf(static_cast<float>(g_app.teleop_v) + g_app.teleop_w, -100.0f, 100.0f);
  l = clampf(l, -cap, cap);
  r = clampf(r, -cap, cap);
  const int16_t pwm_l = static_cast<int16_t>(l * 2.55f);
  const int16_t pwm_r = static_cast<int16_t>(r * 2.55f);

  g_app.req_pwm_l = pwm_l;
  g_app.req_pwm_r = pwm_r;
  // Override (LLD §10.4) only switches the obstacle brake off here; lift/battery/stall never consult it.
  const SafetyInputs in{g_app.lift.lifted(), g_app.lift.tilted(),
                        g_app.batt.level() == BatteryMonitor::Level::kCritical,
                        g_app.safety_override.brakeOn(g_app.envelope.brakeOn()), g_app.us.distMm(), gs.obstacle_stop,
                        0 /* stall_mask: Task 7 */, false /* selftest_locked: Task 7 */};
  const bool was_blocked = g_app.safety.blocked;
  const PwmOutput out = g_app.supervisor.apply(pwm_l, pwm_r, in, g_app.safety);
  port::motor::apply(out);

  if (g_app.safety.blocked && !was_blocked) g_app.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_app.us.distMm()));
  if (!g_app.safety.blocked && was_blocked) g_app.events.push(proto::EventKind::kUnblocked);
  setFast(proto::FastFlagBit::kBlocked, g_app.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_app.safety.halted);
  setFast(proto::FastFlagBit::kLocked, g_app.envelope.lockBrake());
  setFast(proto::FastFlagBit::kSynced, g_app.link.synced());

  g_app.out_pwm_l = out.left();
  g_app.out_pwm_r = out.right();
  if (out.left() != 0 || out.right() != 0) g_app.last_motion_ms = now;
  g_app.moving = !elapsed(now, g_app.last_motion_ms, 300);
  g_app.stationary = !g_app.moving;

  proto::TelemFastMsg& f = g_app.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(l);
  f.spd_cmd_r = static_cast<int8_t>(r);
  f.spd_act_l = -128;
  f.spd_act_r = -128;
  f.flags = g_app.fast_flags;
  g_app.slow.owner = static_cast<uint8_t>(g_app.arbitrator.owner());
  g_app.slow.driving = g_app.moving ? 1 : 0;
}

}  // namespace app
}  // namespace cq
