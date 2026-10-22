// LogoController: carrying out the program's actions (Plan N3, Tank Logo LLD §20.5).
#include <stdio.h>

#include "app/logo_controller.h"
#include "cq_protocol.h"

namespace cq {
namespace app {
namespace {

int16_t servoDd(int16_t deg_from_centre) {  // LOOK degrees (right / up = +) → the Uno's servo angle (0.1°, 900 centre)
  const int32_t dd = 900 - static_cast<int32_t>(deg_from_centre) * 10;  // right and up are the smaller angles
  return static_cast<int16_t>(dd < 0 ? 0 : (dd > 1800 ? 1800 : dd));     // the Uno clamps to its own limits too
}

core::AckedOp op(core::AckedOp::Kind k) {
  core::AckedOp o;
  o.kind = k;
  o.fd = core::kLogoClient;
  return o;
}

core::AckedOp ledOp(const logo::Action& a) {
  core::AckedOp o = op(core::AckedOp::kLed);
  o.a = static_cast<uint8_t>(proto::LedMode::kSolid);
  o.r = a.r;
  o.g = a.g;
  o.b = a.b;
  return o;
}

core::AckedOp soundOp(const logo::Action& a) {  // BEEP = SOUND BLIP (design-notes §102)
  core::AckedOp o = op(core::AckedOp::kSound);
  o.a = a.kind == logo::ActKind::kBeep ? static_cast<uint8_t>(proto::SoundId::kBlip) : a.sound_id;
  return o;
}

core::AckedOp lookOp(const logo::Action& a) {
  core::AckedOp o = op(a.kind == logo::ActKind::kLook ? core::AckedOp::kCamSet : core::AckedOp::kCamHome);
  o.pan_dd = servoDd(a.pan_deg);
  o.tilt_dd = servoDd(a.tilt_deg);
  return o;
}

bool isMotion(logo::ActKind k) {  // + GO / HOLD (LLD §22): the motion layer carries them out too
  return k == logo::ActKind::kMove || k == logo::ActKind::kTurn || k == logo::ActKind::kArc ||
         k == logo::ActKind::kPivot || k == logo::ActKind::kHome || k == logo::ActKind::kGo ||
         k == logo::ActKind::kHold;
}

}  // namespace

void LogoController::carry(const logo::Action& a, uint32_t now) {
  if (isMotion(a.kind)) return motion(a, now);
  switch (a.kind) {  // VARIANT POINT: actions (Tank Logo LLD §10, §20.5)
    case logo::ActKind::kLed: return request(ledOp(a));
    case logo::ActKind::kBeep: case logo::ActKind::kSound: return request(soundOp(a));
    case logo::ActKind::kLook: case logo::ActKind::kLookHome: return request(lookOp(a));
    case logo::ActKind::kTone: return startTone(a, now);
    case logo::ActKind::kSay:
      m_out.all(core::jsonLogoSay(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), a.text));
      m_hold_until = now + kSayPauseMs;
      return;
    default: return speedWhileGoing(a, now);  // SPEED, WAIT, WAIT UNTIL: the RunController's own
  }
}

// LLD §22: SPEED while a GO drives on - GO again at the new speed (the bridge changes it at once).
void LogoController::speedWhileGoing(const logo::Action& a, uint32_t now) {
  if (a.kind != logo::ActKind::kSpeed || !m_rc.going()) return;
  logo::Action go = a;
  go.kind = logo::ActKind::kGo;
  motion(go, now);
}

// A move / turn / arc / pivot / HOME as the program's primitive; refused → the program ends (7021 / 7034).
void LogoController::motion(const logo::Action& a, uint32_t now) {
  note(a);
  core::BridgeReason why = core::BridgeReason::kBodyOffline;
  if (m_h.start_motion(m_fd, a, now, why)) return;
  handle(m_rc.onMotionEvent(why == core::BridgeReason::kUnavailable ? core::MotionEvent::kBodyEncoderFault
                                                                    : core::MotionEvent::kBodyOffline), now);
}

// logo_note: an arc's speed moved into its radius's band.
void LogoController::note(const logo::Action& a) {
  if (a.kind != logo::ActKind::kArc || !a.speed_moved) return;
  char text[96], what[32];
  logo::actionText(a, what, sizeof what);
  snprintf(text, sizeof text, "%s: speed set to %u %% for this radius", what, a.speed_pct);
  m_out.all(core::jsonLogoNote(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), a.line, text));
}

// The program goes on at the Uno's ACK (§102); a full queue is tried again next tick.
void LogoController::request(core::AckedOp o) {
  m_pending = o;
  m_has_pending = true;
  pendingSent();
}

bool LogoController::pendingSent() {
  if (!m_has_pending) return true;
  if (!m_h.uno_request(m_pending)) return false;
  m_has_pending = false;
  m_wait_ack = true;
  return false;
}

// PLAY: TONE frames of at most 10 notes, each sent when the one before has played (a new TONE replaces the tune).
void LogoController::startTone(const logo::Action& a, uint32_t now) {
  m_tone_n = a.n_notes;
  m_tone_i = 0;
  for (uint8_t i = 0; i < a.n_notes; ++i) m_tone[i] = a.notes[i];
  toneChunk(now);
}

void LogoController::toneChunk(uint32_t now) {
  core::AckedOp o = op(core::AckedOp::kTone);
  uint32_t ms = 0;
  while (o.n_notes < kToneChunk && m_tone_i < m_tone_n) {
    const logo::Note& n = m_tone[m_tone_i++];
    o.hz[o.n_notes] = n.hz;
    o.ms[o.n_notes++] = n.ms;
    ms += n.ms;
  }
  m_tone_next = now + ms;
  request(o);
}

}  // namespace app
}  // namespace cq
