#include "core/ws_codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/json_reader.h"
#include "core/ws_codec_internal.h"
#include "core/motion_config.h"
#include "logo/run_controller.h"
#include "cq_protocol.h"

namespace cq {
namespace core {
namespace {

struct KindName {
  const char* name;
  WsCmdKind kind;
};
// VARIANT POINT (LLD R15.6): one row per command name the pages send.
const KindName kKinds[] = {
    {"drive", WsCmdKind::kDrive},          {"stop", WsCmdKind::kStop},
    {"cam_joy", WsCmdKind::kCamJoy},       {"cam_home", WsCmdKind::kCamHome},
    {"release_control", WsCmdKind::kRelease}, {"ping", WsCmdKind::kPing},
    {"hello", WsCmdKind::kHello},          {"sound", WsCmdKind::kSound},
    {"cam_lock", WsCmdKind::kCamLock},     {"audio_mode", WsCmdKind::kAudioMode},
    {"turn", WsCmdKind::kTurn},            {"drive_rate", WsCmdKind::kDriveRate},
    {"cancel", WsCmdKind::kCancel},        {"debug", WsCmdKind::kDebug},
    {"move", WsCmdKind::kMove},            {"calib", WsCmdKind::kCalib},
    {"wifi_status", WsCmdKind::kWifiStatus}, {"wifi_scan", WsCmdKind::kWifiScan},
    {"wifi_save", WsCmdKind::kWifiSave},   {"wifi_forget", WsCmdKind::kWifiForget},
    {"wifi_mode", WsCmdKind::kWifiMode},   {"ap_password", WsCmdKind::kApPassword},
    {"setting", WsCmdKind::kSetting},      {"restart", WsCmdKind::kRestart},
    {"tank", WsCmdKind::kTank},            {"log", WsCmdKind::kLog},
    {"camera_set", WsCmdKind::kCameraSet}, {"selftest", WsCmdKind::kSelfTest},
    {"logo_run", WsCmdKind::kLogoRun}, {"cam_set", WsCmdKind::kCamSet},
    {"logo_step", WsCmdKind::kLogoStep},   {"logo_stop", WsCmdKind::kLogoStop},
    {"skill_things", WsCmdKind::kSkillThings}, {"skill_answer", WsCmdKind::kSkillAnswer},
    {"skill_drive", WsCmdKind::kSkillDrive},   {"skill_done", WsCmdKind::kSkillDone},
    {"skill_failed", WsCmdKind::kSkillFailed}, {"skill_learning", WsCmdKind::kSkillLearning},
};

WsCmdKind kindOf(const char* t) {
  for (const KindName& k : kKinds) {
    if (strcmp(t, k.name) == 0) return k.kind;
  }
  return WsCmdKind::kUnsupported;
}

// proto::SoundId names in lower snake case, in enum order (STARTUP = 1 ... HORN = 24); spec 02 §A9 catalogue.
// The page has the same list (web/basic_drive.html SOUNDS); the count is checked against the generated enum.
const char* const kSoundNames[] = {"startup", "ready", "selftest_fail", "chirp_up", "chirp_down", "blip",
                                   "tick", "jingle", "thinking", "ouch", "error", "success",
                                   "fanfare", "sad", "safety_chirp", "ack_blip", "fail", "confirm",
                                   "question", "ok_chirp", "grumble", "scan", "goodbye", "horn"};
static_assert(sizeof(kSoundNames) / sizeof(kSoundNames[0]) == static_cast<size_t>(proto::SoundId::kHorn),
              "kSoundNames must list every proto::SoundId in order");

uint8_t soundIdOf(const char* name) {
  for (uint8_t i = 0; i < sizeof(kSoundNames) / sizeof(kSoundNames[0]); ++i) {
    if (strcmp(name, kSoundNames[i]) == 0) return static_cast<uint8_t>(i + 1);
  }
  return 0;
}

// audio_mode name -> proto::AudioModeValue; false for a missing or unknown name.
bool audioModeOf(const char* name, uint8_t& out) {
  using M = proto::AudioModeValue;
  const M m = strcmp(name, "normal") == 0 ? M::kNormal : strcmp(name, "quiet") == 0 ? M::kQuiet : M::kMute;
  if (m == M::kMute && strcmp(name, "mute") != 0) return false;
  out = static_cast<uint8_t>(m);
  return true;
}

// ---- per-command checks after all fields were read (fill the derived values; false = invalid) ----
using codec::Seen;
using Check = bool (*)(WsCommand& c, const Seen& s);
bool checkDrive(WsCommand&, const Seen& s) { return s.v && s.w; }
bool checkCamJoy(WsCommand&, const Seen& s) { return s.x && s.y && s.active; }
bool checkPing(WsCommand& c, const Seen&) {
  if (c.t0[0] == '\0') snprintf(c.t0, sizeof(c.t0), "null");
  return true;
}
bool checkSound(WsCommand& c, const Seen& s) {
  c.sound_id = soundIdOf(s.name);  // missing or unknown name -> 0 -> invalid (never an out-of-range SoundId)
  return c.sound_id != 0;
}
bool checkAudioMode(WsCommand& c, const Seen& s) { return audioModeOf(s.mode, c.audio_mode); }
bool checkOn(WsCommand&, const Seen& s) { return s.on; }
bool checkTurn(WsCommand& c, const Seen& s) {  // exactly one of by / to; a pivot has no radius (Plan L)
  return s.mf.by != s.mf.to && c.speed >= motion::kTurnWMin && !(c.pivot && c.radius_cm > 0);
}
bool checkMove(WsCommand& c, const Seen& s) { return s.dist && !s.heading && c.speed >= motion::kMoveSpeedMin; }
bool checkWifiSave(WsCommand&, const Seen& s) { return s.ssid && s.password; }
bool checkWifiMode(WsCommand& c, const Seen& s) {
  c.wifi_ap = strcmp(s.mode, "ap") == 0;
  return c.wifi_ap || strcmp(s.mode, "home") == 0;
}
bool checkApPassword(WsCommand&, const Seen& s) { return s.password; }
bool checkSetting(WsCommand& c, const Seen& s) { return s.setting_id && !c.value_bool; }
bool checkCalib(WsCommand& c, const Seen& s) {
  if (s.reset[0] == '\0') return true;
  c.calib_reset = strcmp(s.reset, "counts_per_m") == 0 ? 0
                  : strcmp(s.reset, "roll") == 0         ? 1
                  : strcmp(s.reset, "gyro_scale") == 0   ? 2
                                                         : -1;
  return c.calib_reset >= 0;
}
// R22: both angles; the servo values as LOOK makes them (right / up are the smaller angles); the range is the
// servos' (pan -90..90, tilt -20..60 - CamLite 300..1100): outside it stays a cam_set, refused with OUT_OF_RANGE.
bool checkCamSet(WsCommand& c, const Seen& s) {
  if (!s.pan || !s.tilt) return false;
  c.cam_in_range = s.pan_d10 >= -900 && s.pan_d10 <= 900 && s.tilt_d10 >= -200 && s.tilt_d10 <= 600;
  if (c.cam_in_range) {
    c.cam_pan_dd = static_cast<int16_t>(900 - s.pan_d10);
    c.cam_tilt_dd = static_cast<int16_t>(900 - s.tilt_d10);
  }
  return true;
}
bool checkDriveRate(WsCommand&, const Seen& s) { return s.v && s.mf.rate; }  // review M20
bool checkCameraSet(WsCommand& c, const Seen& s) {
  c.cam.reset = s.reset_true;
  return c.cam.any();
}
bool checkLogoRun(WsCommand&, const Seen& s) { return s.prog; }

// Plan Z2a: skill messages (bridge LLD R26.2). skill_failed reasons and the SkillEnd each stands for.
struct ReasonRow {
  const char* name;
  logo::SkillEnd end;
};
const ReasonRow kSkillReasons[] = {
    {"not_found", logo::SkillEnd::kNotFound},         {"lost", logo::SkillEnd::kLost},
    {"outside_area", logo::SkillEnd::kOutsideArea},   {"no_way", logo::SkillEnd::kNoWay},
    {"blocked", logo::SkillEnd::kBlocked},            {"unknown_thing", logo::SkillEnd::kUnknownThing},
    {"stale", logo::SkillEnd::kStale},                {"learning", logo::SkillEnd::kLearning},
};
bool checkSkillThings(WsCommand& c, const Seen&) { return c.has_things && c.part >= 1 && c.part <= c.of; }
bool checkSkillAnswer(WsCommand& c, const Seen&) { return c.has_id && c.has_value; }
bool checkSkillDrive(WsCommand& c, const Seen& s) { return c.has_id && s.v && s.mf.rate; }
bool checkSkillDone(WsCommand& c, const Seen&) { return c.has_id; }
bool checkSkillFailed(WsCommand& c, const Seen& s) {
  if (!c.has_id) return false;
  for (const ReasonRow& row : kSkillReasons) {
    if (strcmp(s.reason, row.name) != 0) continue;
    c.skill_end = row.end;
    return true;
  }
  return false;
}

struct CheckRow {
  WsCmdKind kind;
  Check ok;
};
// VARIANT POINT (LLD R15.6): commands without a row need no fields.
const CheckRow kChecks[] = {
    {WsCmdKind::kDrive, checkDrive},         {WsCmdKind::kCamJoy, checkCamJoy},
    {WsCmdKind::kPing, checkPing},           {WsCmdKind::kSound, checkSound},
    {WsCmdKind::kAudioMode, checkAudioMode}, {WsCmdKind::kCamLock, checkOn},
    {WsCmdKind::kTurn, checkTurn},           {WsCmdKind::kMove, checkMove},
    {WsCmdKind::kWifiSave, checkWifiSave},   {WsCmdKind::kWifiMode, checkWifiMode},
    {WsCmdKind::kApPassword, checkApPassword}, {WsCmdKind::kSetting, checkSetting},
    {WsCmdKind::kCalib, checkCalib},         {WsCmdKind::kDriveRate, checkDriveRate},
    {WsCmdKind::kDebug, checkOn},            {WsCmdKind::kCameraSet, checkCameraSet},
    {WsCmdKind::kLogoRun, checkLogoRun},
    {WsCmdKind::kCamSet, checkCamSet},
    {WsCmdKind::kSkillThings, checkSkillThings}, {WsCmdKind::kSkillAnswer, checkSkillAnswer},
    {WsCmdKind::kSkillDrive, checkSkillDrive},   {WsCmdKind::kSkillDone, checkSkillDone},
    {WsCmdKind::kSkillFailed, checkSkillFailed}, {WsCmdKind::kSkillLearning, checkOn},
};

bool complete(WsCommand& c, const codec::Seen& s) {
  c.kind = kindOf(s.t);
  for (const CheckRow& k : kChecks) {
    if (k.kind == c.kind) return k.ok(c, s);
  }
  return true;
}

// The pairs of a non-empty object; false = malformed.
bool readFields(Reader& r, WsCommand& c, Seen& s) {
  do {
    char key[16];
    if (!r.str(key, sizeof(key)) || !r.eat(':') || !codec::readField(key, r, c, s)) return false;
  } while (r.eat(','));
  return true;
}

// A malformed skill message is a bad field (nack BAD_FIELD); anything else stays invalid.
// The "t" is looked up again from the text, so the key order does not matter (a bad field may come before it).
WsCommand refused(const char* text, size_t len) {
  WsCommand bad;
  Reader r(text, len);
  if (!r.eat('{')) return bad;
  do {
    char key[16], t[24];
    if (!r.str(key, sizeof(key)) || !r.eat(':')) return bad;
    if (strcmp(key, "t") != 0) {
      if (!r.skip()) return bad;
      continue;
    }
    if (r.str(t, sizeof(t)) && strncmp(t, "skill_", 6) == 0 && kindOf(t) != WsCmdKind::kUnsupported) {
      bad.kind = WsCmdKind::kBadField;
    }
    return bad;
  } while (r.eat(','));
  return bad;
}

}  // namespace

WsCommand parseWsCommand(const char* text, size_t len) {
  WsCommand c;
  Reader r(text, len);
  Seen s;
  if (!r.eat('{')) return WsCommand();
  if (!r.peek('}') && !readFields(r, c, s)) return refused(text, len);
  if (!r.eat('}') || !r.atEnd() || s.t[0] == '\0') return refused(text, len);
  return complete(c, s) ? c : refused(text, len);
}

}  // namespace core
}  // namespace cq
