#include "domain/lite/lite_frames.h"
#include "domain/ui/catalogues.h"
#include "lite/lite_app.h"
#include "ports/clock.h"

namespace cq {
namespace lite {

namespace {

void rejectEvent(uint8_t frame_type, uint8_t src, proto::AckStatus st) {
  if (g_lite.rejects.allow(src, port::clock::nowMs())) {
    g_lite.events.push(proto::EventKind::kRejected, frame_type, static_cast<int16_t>(st));
  }
}

proto::AckStatus networkMotion(uint8_t frame_type, uint8_t src) {
  if (g_lite.self_test.locked()) {  // LLD §10.9: critical self-test → every motion command refused
    rejectEvent(frame_type, src, proto::AckStatus::kBlocked);
    return proto::AckStatus::kBlocked;
  }
  const proto::AckStatus st = g_lite.arbitrator.requestNetwork(true, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) rejectEvent(frame_type, src, st);
  return st;
}

template <typename Msg>
bool dec(const uint8_t* p, uint8_t len, Msg& m) { return proto::decode(p, len, m); }

proto::AckStatus handleLink(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  constexpr proto::AckStatus kBad = proto::AckStatus::kInvalidCommand;
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: { proto::HeartbeatMsg m{}; return dec(p, len, m) ? proto::AckStatus::kOk : kBad; }
    case F::kHelloAck: { proto::HelloAckMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.link.onHelloAck(); return proto::AckStatus::kOk; }
    case F::kHelloReq: { proto::HelloReqMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.link.onHelloReq(); return proto::AckStatus::kOk; }
    case F::kEspAck: { proto::EspAckMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.link.onEspAck(m.ack_seq); return proto::AckStatus::kOk; }
    default: return proto::AckStatus::kUnavailable;
  }
}

proto::AckStatus handleMotion(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  constexpr proto::AckStatus kBad = proto::AckStatus::kInvalidCommand;
  switch (static_cast<F>(type)) {
    case F::kDrive: {
      proto::DriveMsg m{};
      if (!dec(p, len, m)) return kBad;
      const proto::AckStatus st = networkMotion(type, m.src);
      if (st != proto::AckStatus::kOk) return st;
      g_lite.deadman_latched = false;
      g_lite.teleop_v = clampDriveLite(m.v);  // ±100 (the decoder accepts −128)
      g_lite.teleop_w = clampDriveLite(m.w);
      g_lite.teleop_auto = m.src == static_cast<uint8_t>(proto::Source::kAuto);
      return st;
    }
    case F::kStop: {
      proto::StopMsg m{};
      if (!dec(p, len, m)) return kBad;
      g_lite.teleop_v = 0;
      g_lite.teleop_w = 0;
      g_lite.arbitrator.onNetworkStop();
      return proto::AckStatus::kOk;
    }
    case F::kBrake: { proto::BrakeMsg m{}; if (!dec(p, len, m)) return kBad; return g_lite.envelope.setBrake(m.on != 0); }
    case F::kEnvelope: {
      proto::EnvelopeMsg m{};
      if (!dec(p, len, m)) return kBad;
      const proto::AckStatus st = g_lite.envelope.set(m.speed_cap, m.accel_cap, m.lock_brake);
      if (st == proto::AckStatus::kOk) {
        const uint16_t packed = static_cast<uint16_t>(m.speed_cap | (static_cast<uint16_t>(m.accel_cap) << 8u));
        g_lite.events.push(proto::EventKind::kEnvelopeChanged, static_cast<int16_t>(packed), g_lite.envelope.flags());
        g_lite.slow.env_speed_cap = m.speed_cap;
        g_lite.slow.env_accel_cap = m.accel_cap;
        g_lite.slow.env_lock_brake = g_lite.envelope.flags();
      }
      return st;
    }
    case F::kOwnerHint: { proto::OwnerHintMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.owner_hint = static_cast<proto::OwnerHintValue>(m.owner); return proto::AckStatus::kOk; }
    default: return proto::AckStatus::kUnavailable;
  }
}

proto::AckStatus handleCamera(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  constexpr proto::AckStatus kBad = proto::AckStatus::kInvalidCommand;
  const uint32_t now = port::clock::nowMs();
  switch (static_cast<F>(type)) {
    case F::kCamJoy: { proto::CamJoyMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.joy(m.x, m.y, m.active != 0, now); return proto::AckStatus::kOk; }
    case F::kCamSet: { proto::CamSetMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.set(m.pan, m.tilt); return proto::AckStatus::kOk; }
    case F::kCamNudge: { proto::CamNudgeMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.nudge(m.dpan, m.dtilt); return proto::AckStatus::kOk; }
    case F::kCamHome: { proto::CamHomeMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.home(); return proto::AckStatus::kOk; }
    case F::kCamLock: { proto::CamLockMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.lock(m.on != 0); return proto::AckStatus::kOk; }
    default: return proto::AckStatus::kUnavailable;
  }
}

proto::AckStatus handleSettings(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  constexpr proto::AckStatus kBad = proto::AckStatus::kInvalidCommand;
  switch (static_cast<F>(type)) {
    case F::kSettingSet: {
      proto::SettingSetMsg m{};
      if (!dec(p, len, m)) return kBad;
      const proto::AckStatus st = g_lite.settings.set(m.id, m.value);
      if (st == proto::AckStatus::kOk) applySettings();
      return st;
    }
    case F::kSettingGet: {
      proto::SettingGetMsg m{};
      if (!dec(p, len, m)) return kBad;
      int32_t v = 0;
      if (!g_lite.settings.get(m.id, v)) return proto::AckStatus::kUnavailable;
      g_lite.setting_value.id = m.id;
      g_lite.setting_value.value = v;
      g_lite.setting_value_pending = true;
      return proto::AckStatus::kOk;
    }
    case F::kSettingsSave: {
      proto::SettingsSaveMsg m{};
      if (!dec(p, len, m)) return kBad;
      if (g_lite.save_requested || !g_lite.eewriter.idle()) return proto::AckStatus::kSettingsSaveBusy;
      g_lite.save_requested = true;
      return proto::AckStatus::kOk;
    }
    default: return proto::AckStatus::kUnavailable;
  }
}

proto::AckStatus handleUi(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  constexpr proto::AckStatus kBad = proto::AckStatus::kInvalidCommand;
  const uint32_t now = port::clock::nowMs();
  switch (static_cast<F>(type)) {
    case F::kLed: {
      proto::LedMsg m{};
      if (!dec(p, len, m) || m.mode > static_cast<uint8_t>(proto::LedMode::kPulse)) return kBad;
      g_lite.led.set(LedLayer::kCustom, {static_cast<proto::LedMode>(m.mode), {m.r, m.g, m.b}, m.period_ms, m.duration_ms}, now);
      return proto::AckStatus::kOk;
    }
    case F::kSound: {
      proto::SoundMsg m{};
      if (!dec(p, len, m)) return kBad;
      Note n[SoundSequencer::kMaxNotes];
      const uint8_t count = soundNotes(static_cast<proto::SoundId>(m.id), n);
      if (count == 0) return kBad;
      return g_lite.sound.play(n, count, SoundSequencer::kPrioEvent, now) ? proto::AckStatus::kOk : proto::AckStatus::kBusy;
    }
    case F::kTone: {
      proto::ToneMsg m{};
      if (!dec(p, len, m) || m.n == 0) return kBad;
      Note n[SoundSequencer::kMaxNotes];
      for (uint8_t i = 0; i < m.n; ++i) n[i] = Note{m.notes[i].hz, m.notes[i].ms};
      return g_lite.sound.play(n, m.n, SoundSequencer::kPrioEvent, now) ? proto::AckStatus::kOk : proto::AckStatus::kBusy;
    }
    case F::kAudioMode: {
      proto::AudioModeMsg m{};
      if (!dec(p, len, m) || m.mode > static_cast<uint8_t>(proto::AudioModeValue::kMute)) return kBad;
      g_lite.sound.setMode(static_cast<proto::AudioModeValue>(m.mode));
      return proto::AckStatus::kOk;
    }
    default: return proto::AckStatus::kUnavailable;
  }
}

// VARIANT POINT: the frame families of the Lite firmware (lite_frames.cpp decides which types are handled at all).
proto::AckStatus handle(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: case F::kHelloAck: case F::kHelloReq: case F::kEspAck: return handleLink(type, p, len);
    case F::kDrive: case F::kStop: case F::kBrake: case F::kEnvelope: case F::kOwnerHint: return handleMotion(type, p, len);
    case F::kCamJoy: case F::kCamSet: case F::kCamNudge: case F::kCamHome: case F::kCamLock: return handleCamera(type, p, len);
    case F::kSettingSet: case F::kSettingGet: case F::kSettingsSave: return handleSettings(type, p, len);
    case F::kLed: case F::kSound: case F::kTone: case F::kAudioMode: return handleUi(type, p, len);
    default: return proto::AckStatus::kUnavailable;
  }
}

}  // namespace

bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st) {
  switch (lite_frames::frameUse(type)) {
    case lite_frames::FrameUse::kUnknown: ++g_lite.parser.errors().unknown_type; return false;
    case lite_frames::FrameUse::kUnavailable: st = proto::AckStatus::kUnavailable; return true;
    case lite_frames::FrameUse::kHandled: st = handle(type, p, len); return lite_frames::needsAck(type);
  }
  return false;
}

void applySettings() {
  const LiteSettings& s = g_lite.settings.values();
  g_lite.encoders_enabled = s.encoders_enabled != 0;
  g_lite.gyro_zero.seed(s.gyro_bias_z);
  g_lite.heading.configure(headingGainsFromMilli(s.heading_kp_milli, s.rate_kp_milli));
}

}  // namespace lite
}  // namespace cq
