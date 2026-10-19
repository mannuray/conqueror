#include "core/ws_codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/json_reader.h"
#include "core/ws_codec_internal.h"
#include "core/motion_config.h"
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
    {"app_set", WsCmdKind::kAppSet},       {"logo_run", WsCmdKind::kLogoRun},
    {"logo_step", WsCmdKind::kLogoStep},   {"logo_stop", WsCmdKind::kLogoStop},
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
bool checkSetting(WsCommand&, const Seen& s) { return s.setting_id; }
bool checkCalib(WsCommand& c, const Seen& s) {
  if (s.reset[0] == '\0') return true;
  c.calib_reset = strcmp(s.reset, "counts_per_m") == 0 ? 0 : (strcmp(s.reset, "roll") == 0 ? 1 : -1);
  return c.calib_reset >= 0;
}
bool checkDriveRate(WsCommand&, const Seen& s) { return s.v && s.mf.rate; }  // review M20
bool checkCameraSet(WsCommand& c, const Seen& s) {
  c.cam.reset = s.reset_true;
  return c.cam.any();
}
bool checkAppSet(WsCommand& c, const Seen& s) { return c.has_ai_url && !s.url_bad; }
bool checkLogoRun(WsCommand&, const Seen& s) { return s.prog; }

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
    {WsCmdKind::kAppSet, checkAppSet},       {WsCmdKind::kLogoRun, checkLogoRun},
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

bool urlCharOk(char ch) {
  return ch > ' ' && ch <= '~' && strchr("\"\\<>'", ch) == nullptr;
}

}  // namespace

WsCommand parseWsCommand(const char* text, size_t len) {
  WsCommand c;
  const WsCommand bad;
  Reader r(text, len);
  Seen s;
  if (!r.eat('{')) return bad;
  if (!r.peek('}') && !readFields(r, c, s)) return bad;
  if (!r.eat('}') || !r.atEnd() || s.t[0] == '\0') return bad;
  return complete(c, s) ? c : bad;
}

bool appUrlValid(const char* url) {
  const size_t n = strlen(url);
  if (n == 0) return true;
  if (n > 96 || n < 8 || strncmp(url, "http://", 7) != 0) return false;
  for (size_t i = 0; i < n; ++i) {
    if (!urlCharOk(url[i])) return false;
  }
  return true;
}

}  // namespace core
}  // namespace cq
