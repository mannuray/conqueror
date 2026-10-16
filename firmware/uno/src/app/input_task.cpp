#include "app/app.h"
#include "app/motion_api.h"
#include "domain/input/boot_button.h"
#include "domain/input/ir_controller.h"
#include "domain/ui/catalogues.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "app/tasks.h"

namespace cq {
namespace app {
namespace {

constexpr float kJogStepDeg = 1.2f;  // per 20 ms tick = 60 °/s

IrController g_ir;
BootButton g_button;
int8_t g_jog_pan = 0;
int8_t g_jog_tilt = 0;

void play(proto::SoundId id, uint8_t prio = SoundSequencer::kPrioEvent) {
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(id, n);
  g_app.sound.play(n, count, prio, port::clock::nowMs());
}

void beeps(uint8_t count, uint16_t hz) {
  Note n[SoundSequencer::kMaxNotes];
  uint8_t k = 0;
  for (uint8_t i = 0; i < count && k + 1 < SoundSequencer::kMaxNotes; ++i) {
    n[k++] = Note{hz, 80};
    n[k++] = Note{0, 120};
  }
  g_app.sound.play(n, k, SoundSequencer::kPrioEvent, port::clock::nowMs());
}

void batteryCheck(uint32_t now) {
  Rgb c{40, 40, 40};
  uint8_t count = 0;
  switch (g_app.batt.level()) {
    case BatteryMonitor::Level::kOk: c = Rgb{0, 150, 0}; count = 3; break;
    case BatteryMonitor::Level::kLow: c = Rgb{160, 80, 0}; count = 2; break;
    case BatteryMonitor::Level::kCritical: c = Rgb{255, 0, 0}; count = 1; break;
    case BatteryMonitor::Level::kUsb: break;  // no pack: white, no beeps
  }
  g_app.led.set(LedLayer::kAttention, {proto::LedMode::kSolid, c, 0, 2000}, now);
  if (count > 0) beeps(count, 1500);
}

void applyIr(const IrOutput& o, uint32_t now) {
  const proto::PrimitiveKind prim = g_app.primitives.active();
  if (o.active) {
    if (prim != proto::PrimitiveKind::kNone) g_app.primitives.cancel(proto::AckStatus::kCancelled);
    g_app.teleop_rate_mode = false;
    g_app.teleop_v = o.v;
    g_app.teleop_w = o.w;
    g_app.teleop_auto = false;  // IR: a driver's input
    if (o.v != 0 || o.w != 0) g_app.safety_override.onIrMotion(true, now);  // IR driving keeps Override alive
  } else {
    stopTeleop();  // also ends DRIVE_RATE mode: no spin may survive under IR ownership
  }
  g_jog_pan = o.jog_pan;
  g_jog_tilt = o.jog_tilt;
  if (o.stop) {
    stopTeleop();
    g_app.primitives.cancel(proto::AckStatus::kCancelled);
    g_app.arbitrator.releaseIr();
    g_app.safety_override.end(proto::OverrideReason::kStop);  // IR OK = STOP: ends Override
    drainOverride();
  }
  if (o.cam_home) g_app.camera.home();
  if (o.speed_changed) {
    const Note n{static_cast<uint16_t>(400 + g_ir.speedPct() * 10), 60};
    g_app.sound.play(&n, 1, SoundSequencer::kPrioEvent, now);
  }
  if (o.toggle_mute) {
    const bool muted = g_app.sound.mode() == proto::AudioModeValue::kMute;
    g_app.sound.setMode(muted ? proto::AudioModeValue::kNormal : proto::AudioModeValue::kMute);
    if (muted) play(proto::SoundId::kBlip);
    ++g_app.toggle_seq;  // 3 blinks: green = mute on, red = off (LLD §10.6)
    g_app.toggle_on = !muted;
  }
  if (o.battery_check) batteryCheck(now);
  if (o.turn_180) {
    if (startLocalTurn(1800, g_ir.speedPct()) == proto::AckStatus::kOk) {
      g_app.safety_override.onIrMotion(true, now);
    } else {
      play(proto::SoundId::kError);
    }
  }
  if (o.toggle_parking) {
    int32_t v = 0;
    g_app.settings.get(static_cast<uint8_t>(proto::SettingId::kParkingBeeps), v);
    g_app.settings.set(static_cast<uint8_t>(proto::SettingId::kParkingBeeps), v ? 0 : 1);  // RAM until SETTINGS_SAVE
    play(v ? proto::SoundId::kChirpDown : proto::SoundId::kChirpUp);
    ++g_app.toggle_seq;
    g_app.toggle_on = v == 0;
  }
  if (o.toggle_override) {  // `*` held 2 s (LLD §10.5); IR took ownership on the first `*` frame
    const proto::AckStatus st = g_app.safety_override.requestIr(overrideGate(), now);
    drainOverride();  // event + override_seq → chirp and green/red blinks (Task 13)
    if (st != proto::AckStatus::kOk) {  // envelope (child play), latched fault, or still network-bound
      play(proto::SoundId::kGrumble);
      ++g_app.toggle_seq;
      g_app.toggle_on = false;
    }
  }
  if (o.cal_request) {
    if (g_app.link.alive(now) && !g_app.self_test.locked()) {  // LLD §10.9: no calibration while locked
      g_app.events.push(proto::EventKind::kCalRequest);
      play(proto::SoundId::kBlip);
    } else {
      play(proto::SoundId::kError);
    }
  }
}

}  // namespace

// 50 Hz.
void inputTask() {
  const uint32_t now = port::clock::nowMs();
  IrOutput out{};
  if (g_app.ir_pending) {
    g_app.ir_pending = false;
    if (g_ir.onEvent(g_app.ir_event, out)) {
      // IR always wins and works with no ESP32 at all. On takeover end network primitives and teleop
      // (incl. DRIVE_RATE) first, so the key applied next (e.g. `0` → TURN 180°) is not cancelled.
      if (g_app.arbitrator.onIrInput(now)) {
        g_app.primitives.cancel(proto::AckStatus::kCancelled);
        stopTeleop();
      }
      applyIr(out, now);
    }
  }
  if (g_ir.tick(now, out)) applyIr(out, now);
  if (g_jog_pan != 0 || g_jog_tilt != 0) {
    g_app.camera.nudge(-g_jog_pan * kJogStepDeg, -g_jog_tilt * kJogStepDeg);  // right / up = lower servo angle (§45)
  }

  switch (g_button.update(port::button::pressed(), now)) {
    case BootButton::Action::kPress:
      g_app.events.push(proto::EventKind::kButtonPress);
      play(proto::SoundId::kBlip);
      break;
    case BootButton::Action::kWifiThreshold:
    case BootButton::Action::kAdminThreshold:
      play(proto::SoundId::kTick, SoundSequencer::kPrioAlarm);
      break;
    case BootButton::Action::kWifiReset:
      g_app.link.setRequest(static_cast<uint8_t>(proto::FrameType::kWifiResetReq));
      play(proto::SoundId::kJingle, SoundSequencer::kPrioAlarm);
      g_app.led.set(LedLayer::kAttention, {proto::LedMode::kBlink, Rgb{0, 0, 255}, 200, 3000}, now);
      break;
    case BootButton::Action::kAdminReset:
      g_app.link.setRequest(static_cast<uint8_t>(proto::FrameType::kAdminResetReq));
      play(proto::SoundId::kJingle, SoundSequencer::kPrioAlarm);
      g_app.led.set(LedLayer::kAttention, {proto::LedMode::kBlink, Rgb{255, 0, 255}, 200, 3000}, now);
      break;
    case BootButton::Action::kNone:
      break;
  }
}

}  // namespace app
}  // namespace cq
