#include "app/app.h"
#include "app/tasks.h"
#include "app/ui_api.h"
#include "domain/ui/catalogues.h"
#include "ports/clock.h"
#include "ports/led.h"
#include "ports/tone.h"

namespace cq {
namespace app {

void uiInit() {
  port::tone::begin();
  port::led::begin();
}

void uiTask() {
  const uint32_t now = port::clock::nowMs();
  const GlobalSettings& gs = g_app.settings.global();
  AnnState s{};
  s.synced = g_app.link.synced();
  s.calibrating = g_app.cal.running();
  s.blocked = g_app.safety.blocked;
  s.lifted = g_app.lift.lifted();
  s.tilted = g_app.lift.tilted();
  s.batt_low = g_app.batt.level() == BatteryMonitor::Level::kLow;
  s.batt_crit = g_app.batt.level() == BatteryMonitor::Level::kCritical;
  s.selftest_critical = g_app.self_test.locked();
  s.reversing = g_app.out_pwm_l < 0 && g_app.out_pwm_r < 0;
  s.forward = g_app.out_pwm_l > 0 && g_app.out_pwm_r > 0;
  s.parking_beeps = gs.parking_beeps != 0;
  s.owner = g_app.arbitrator.owner();
  s.hint = g_app.owner_hint;
  s.bump_seq = g_app.bump_seq;
  s.selftest_runs = g_app.self_test.runs();
  s.selftest_failed = g_app.self_test.failMask() != 0;
  s.override_on = g_app.safety_override.active();
  s.override_seq = g_app.override_seq;
  s.toggle_seq = g_app.toggle_seq;
  s.toggle_on = g_app.toggle_on;
  s.driving = g_app.moving;
  const int16_t pl = g_app.out_pwm_l < 0 ? static_cast<int16_t>(-g_app.out_pwm_l) : g_app.out_pwm_l;
  const int16_t pr = g_app.out_pwm_r < 0 ? static_cast<int16_t>(-g_app.out_pwm_r) : g_app.out_pwm_r;
  s.speed_pct = static_cast<uint8_t>((pl > pr ? pl : pr) * 100 / 255);
  g_app.led.setMax(gs.led_max);  // LLD §10.6: brightness cap (default 50/255, as the old firmware)
  g_app.annunciator.update(s, now, g_app.led, g_app.sound);

  uint16_t hz = 0;
  if (g_app.sound.tick(now, hz)) {
    if (hz == 0) {
      port::tone::stop();
    } else {
      port::tone::play(hz);
    }
  }
  const Rgb c = g_app.led.output(now);
  if (c != g_app.led_out) {
    port::led::write(c);
    g_app.led_out = c;
  }
}

proto::AckStatus onLed(const uint8_t* p, uint8_t len) {
  proto::LedMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (m.mode > static_cast<uint8_t>(proto::LedMode::kPulse)) return proto::AckStatus::kInvalidCommand;
  g_app.led.set(LedLayer::kCustom, {static_cast<proto::LedMode>(m.mode), {m.r, m.g, m.b}, m.period_ms, m.duration_ms},
                port::clock::nowMs());
  return proto::AckStatus::kOk;
}

proto::AckStatus onSound(const uint8_t* p, uint8_t len) {
  proto::SoundMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(static_cast<proto::SoundId>(m.id), n);
  if (count == 0) return proto::AckStatus::kInvalidCommand;
  return g_app.sound.play(n, count, SoundSequencer::kPrioEvent, port::clock::nowMs()) ? proto::AckStatus::kOk
                                                                                       : proto::AckStatus::kBusy;
}

proto::AckStatus onTone(const uint8_t* p, uint8_t len) {
  proto::ToneMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (m.n == 0) return proto::AckStatus::kInvalidCommand;  // nothing to play (play() would answer BUSY)
  Note n[SoundSequencer::kMaxNotes];
  for (uint8_t i = 0; i < m.n; ++i) n[i] = Note{m.notes[i].hz, m.notes[i].ms};
  return g_app.sound.play(n, m.n, SoundSequencer::kPrioEvent, port::clock::nowMs()) ? proto::AckStatus::kOk
                                                                                     : proto::AckStatus::kBusy;
}

proto::AckStatus onAudioMode(const uint8_t* p, uint8_t len) {
  proto::AudioModeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (m.mode > static_cast<uint8_t>(proto::AudioModeValue::kMute)) return proto::AckStatus::kInvalidCommand;
  g_app.sound.setMode(static_cast<proto::AudioModeValue>(m.mode));
  if (m.mode == static_cast<uint8_t>(proto::AudioModeValue::kMute)) port::tone::stop();
  return proto::AckStatus::kOk;
}

}  // namespace app
}  // namespace cq
