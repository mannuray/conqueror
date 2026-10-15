#include "domain/ui/annunciator.h"

#include "domain/ui/catalogues.h"
#include "domain/util/math_util.h"

namespace cq {

void Annunciator::sound(proto::SoundId id, uint8_t prio, uint32_t now_ms, SoundSequencer& snd) {
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(id, n);
  snd.play(n, count, prio, now_ms);
}

void Annunciator::pickAttention(const AnnState& s, uint32_t now_ms, LedSequencer& led) {
  if (s.blocked) {
    led.set(LedLayer::kAttention, {proto::LedMode::kBlink, kColorAlarm, 200, 0}, now_ms);
  } else if (s.override_on) {
    led.set(LedLayer::kAttention, {proto::LedMode::kPulse, kColorOverride, 1000, 0}, now_ms);
  } else {
    led.clear(LedLayer::kAttention);
  }
}

void Annunciator::update(const AnnState& s, uint32_t now_ms, LedSequencer& led, SoundSequencer& snd) {
  const AnnState p = m_primed ? m_prev : AnnState{};
  m_primed = true;

  // sounds on edges
  if (s.selftest_runs != p.selftest_runs && s.selftest_runs != 0) {  // boot run and every re-run (LLD §10.9)
    const proto::SoundId ok_id = s.selftest_runs == 1 ? proto::SoundId::kStartup : proto::SoundId::kSuccess;
    sound(s.selftest_failed ? proto::SoundId::kSelftestFail : ok_id, SoundSequencer::kPrioEvent, now_ms, snd);
  }
  if (s.synced && !p.synced) sound(proto::SoundId::kReady, SoundSequencer::kPrioInfo, now_ms, snd);
  // Tilt siren = the old safety chirp, repeated while tilted (LLD §10.6); a bump keeps its "ouch".
  if (s.tilted && (!p.tilted || elapsed(now_ms, m_last_siren_ms, kTiltSirenMs))) {
    m_last_siren_ms = now_ms;
    sound(proto::SoundId::kSafetyChirp, SoundSequencer::kPrioAlarm, now_ms, snd);
  }
  if (s.bump_seq != p.bump_seq) sound(proto::SoundId::kOuch, SoundSequencer::kPrioAlarm, now_ms, snd);
  if (s.lifted && !p.lifted) sound(proto::SoundId::kBlip, SoundSequencer::kPrioEvent, now_ms, snd);
  if (s.batt_low && !p.batt_low) sound(proto::SoundId::kError, SoundSequencer::kPrioEvent, now_ms, snd);
  if (s.batt_crit && (!p.batt_crit || elapsed(now_ms, m_last_crit_ms, 30000))) {
    m_last_crit_ms = now_ms;
    sound(proto::SoundId::kError, SoundSequencer::kPrioAlarm, now_ms, snd);
  }
  if (s.blocked && !p.blocked) sound(proto::SoundId::kSafetyChirp, SoundSequencer::kPrioEvent, now_ms, snd);
  // Safety Override enter/exit: chirp at alarm priority (plays in Quiet) + toggle blinks (below).
  if (s.override_seq != p.override_seq) {
    sound(s.override_on ? proto::SoundId::kChirpUp : proto::SoundId::kChirpDown, SoundSequencer::kPrioAlarm, now_ms,
          snd);
  }
  if (s.owner == proto::Owner::kIr && p.owner != proto::Owner::kIr) sound(proto::SoundId::kBlip, SoundSequencer::kPrioInfo, now_ms, snd);
  if (s.forward && !p.forward) sound(proto::SoundId::kChirpUp, SoundSequencer::kPrioInfo, now_ms, snd);
  if (s.reversing && !p.reversing) {
    sound(proto::SoundId::kChirpDown, SoundSequencer::kPrioInfo, now_ms, snd);
    m_last_beep_ms = now_ms;
  }
  if (s.reversing && s.parking_beeps && elapsed(now_ms, m_last_beep_ms, kParkingBeepMs)) {
    m_last_beep_ms = now_ms;
    sound(proto::SoundId::kTick, SoundSequencer::kPrioInfo, now_ms, snd);
  }

  // LED layers (only re-set on change so blink phases stay steady). Any change of an alarm input re-picks the
  // pattern: e.g. lifted clears while batt_crit stays → the 1000 ms batt_crit blink takes over.
  if (s.tilted != p.tilted || s.lifted != p.lifted || s.batt_crit != p.batt_crit || s.batt_low != p.batt_low ||
      s.selftest_critical != p.selftest_critical || s.usb_power != p.usb_power) {
    if (s.selftest_critical) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kSolid, kColorAlarm, 0, 0}, now_ms);
    } else if (s.tilted || s.lifted) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kColorAlarm, 250, 0}, now_ms);
    } else if (s.batt_crit) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kColorAlarm, 1000, 0}, now_ms);
    } else if (s.batt_low) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kColorLow, 2000, 0, kLowFlashMs}, now_ms);
    } else if (s.usb_power) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kColorUsb, 2000, 0, kLowFlashMs}, now_ms);
    } else {
      led.clear(LedLayer::kAlarm);
    }
  }
  // Attention layer: toggle-confirm blinks > obstacle-stop red blink (100/100 ms) > Override pulse > none.
  const bool toggled = s.toggle_seq != p.toggle_seq || s.override_seq != p.override_seq;
  if (toggled) {
    const bool on = s.override_seq != p.override_seq ? s.override_on : s.toggle_on;
    led.set(LedLayer::kAttention, {proto::LedMode::kBlink, on ? kColorOn : kColorAlarm, 160, kToggleBlinkMs}, now_ms);
    m_toggle_active = true;
    m_toggle_since_ms = now_ms;
  } else if (m_toggle_active && elapsed(now_ms, m_toggle_since_ms, kToggleBlinkMs)) {
    m_toggle_active = false;
    pickAttention(s, now_ms, led);
  } else if (!m_toggle_active &&
             (s.blocked != p.blocked || s.override_on != p.override_on ||
              ((s.blocked || s.override_on) && !led.active(LedLayer::kAttention)))) {
    // also after a one-shot attention pattern (e.g. the IR `9` battery check) expired: the obstacle blink /
    // Override pulse must come back while its cause holds
    pickAttention(s, now_ms, led);
  }
  if (s.calibrating != p.calibrating) {
    if (s.calibrating) {
      led.set(LedLayer::kStatus, {proto::LedMode::kBlink, kColorCal, 500, 0}, now_ms);
    } else {
      led.clear(LedLayer::kStatus);
    }
  }
  if (s.owner != p.owner || s.hint != p.hint || s.synced != p.synced || !m_prev_base_set) {
    m_prev_base_set = true;
    LedPattern base{proto::LedMode::kSolid, kColorReady, 0, 0};
    if (s.owner == proto::Owner::kIr) {
      base.color = kColorIr;
    } else if (s.owner == proto::Owner::kNetwork) {
      base.color = s.hint == proto::OwnerHintValue::kAi ? kColorAi : kColorHuman;
    } else if (!s.synced) {
      base.color = kColorNoLink;  // solid base: the sequencer makes it breathe while idle
    }
    led.set(LedLayer::kBase, base, now_ms);
  }
  // Base colour: breathes when idle (2 s, 1 s on a critical battery), brightness rises with speed when driving.
  const uint8_t speed = s.speed_pct > 100 ? 100 : s.speed_pct;
  led.setBaseStyle(!s.driving, s.batt_crit ? 1000 : 2000,
                   static_cast<uint8_t>(LedSequencer::kMinLevelPct +
                                        (100u - LedSequencer::kMinLevelPct) * speed / 100u));
  m_prev = s;
}

}  // namespace cq
