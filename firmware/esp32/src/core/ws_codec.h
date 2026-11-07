// WebSocket JSON in -> WsCommand (bridge LLD R2.9, spec 01 §4-5 names). Messages are small flat
// objects; this is a fixed-buffer reader for exactly that (no JSON library, no heap). Unknown keys
// (e.g. a gateway "key") are skipped, nested values are skipped, numbers are clamped to -100..100.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace logo {
enum class SkillEnd : uint8_t;  // logo/run_controller.h
}
namespace core {

enum class WsCmdKind : uint8_t {
  kHello,
  kDrive,
  kStop,
  kCamJoy,
  kCamHome,
  kRelease,
  kPing,
  kSound,    // Plan F (spec 01 §4): {"t":"sound","name":"<catalogue name>"} -> SOUND
  kCamLock,  // {"t":"cam_lock","on":bool} -> CAM_LOCK
  kAudioMode,  // R3.1: {"t":"audio_mode","mode":"normal"|"quiet"|"mute"} -> AUDIO_MODE
  kTurn,       // Plan H (LLD R4.2): {"t":"turn","by"|"to":deg,"speed"?,"radius_cm"?} -> motion service / TURN
  kDriveRate,  // {"t":"drive_rate","v":%,"yaw_rate":deg/s} (stream) -> motion service / DRIVE_RATE
  kCancel,     // {"t":"cancel"} -> ends the running primitive
  kDebug,      // LLD R5: {"t":"debug","on":bool,"hz"?:10|50} -> this client gets td / tds
  kMove,       // Plan K (LLD R7.1): {"t":"move","dist_cm":±1..500,"speed"?:10..100} -> motion service
  kCalib,      // {"t":"calib","counts_per_m"?:3000..15000} -> read / set the bridge's counts_per_m (NVS)
  kWifiStatus,  // Plan M (spec 02 §I3): {"t":"wifi_status"} -> wifi_status
  kWifiScan,    // {"t":"wifi_scan"} -> wifi_scan (async)
  kWifiSave,    // {"t":"wifi_save","slot"?:1-3,"ssid","password"} -> confirm
  kWifiForget,  // {"t":"wifi_forget","slot"?} -> confirm (no slot = all)
  kWifiMode,    // {"t":"wifi_mode","mode":"home"|"ap"} -> confirm
  kApPassword,  // {"t":"ap_password","password"} -> confirm
  kSetting,     // {"t":"setting","setting_id","value"?} -> setting (set: confirm)
  kRestart,     // {"t":"restart"} -> ack, restart after 0.5 s
  kTank,        // Plan O (LLD R13): {"t":"tank"} -> tank (self-test, camera, AI address)
  kLog,         // {"t":"log","since"?:n} -> log (at most 25 entries)
  kCameraSet,   // {"t":"camera_set","size"?,"quality"?,"fps_cap"?,"brightness"?,"contrast"?,"low_light"?,"vflip"?,"hmirror"?,"reset"?}
  kSelfTest,    // {"t":"selftest"} -> CALIBRATE SELF_TEST on the Uno
  kLogoRun,     // Plan N3 (Tank Logo LLD §20.4): {"t":"logo_run","prog":n,"step"?:bool} -> run a checked program
  kLogoStep,    // {"t":"logo_step"} -> the next line in step mode
  kLogoStop,    // {"t":"logo_stop"} -> end the running program (7040)
  kCamSet,      // bridge LLD R22: {"t":"cam_set","pan":deg,"tilt":deg} -> CAM_SET (degrees from the centre)
  // Plan Z2a (bridge LLD R26.2): the skill server's messages; a bad field in one of them is kBadField (nack BAD_FIELD)
  kSkillThings,  // {"t":"skill_things","things":[...]} - read the list with parseThingList
  kSkillAnswer,  // {"t":"skill_answer","id":n,"value":true|false|cm}
  kSkillDrive,   // {"t":"skill_drive","id":n,"v":%,"yaw_rate":deg/s}
  kSkillDone,    // {"t":"skill_done","id":n}
  kSkillFailed,  // {"t":"skill_failed","id":n,"reason":r,"thing"?}
  kSkillLearning,  // Plan Z2b-1 (R27.2): {"t":"skill_learning","on":bool} around a teach -> Caps::ai_learning (7056)
  kBadField,     // a skill message with a malformed or missing field -> nack BAD_FIELD
  kUnsupported,  // valid JSON, "t" not in the Basic Drive slice -> nack INVALID_COMMAND
  kInvalid,      // not JSON / no "t" / bad field -> nack INVALID_COMMAND
};

// camera_set fields (Plan O): size 0 QVGA, 1 VGA, 2 SVGA, 3 HD; quality 10..40; fps 0 (max), 5, 10, 15.
struct CamFields {
  bool has_size = false, has_quality = false, has_fps = false, has_bright = false, has_contrast = false;
  bool has_low = false, has_vflip = false, has_hmirror = false, reset = false;
  bool has_wb = false, has_night = false;  // R24
  uint8_t wb = 0;
  bool night = false;
  uint8_t size = 1, quality = 12, fps = 0;
  int8_t bright = 0, contrast = 0;
  bool low = false, vflip = true, hmirror = false;
  bool any() const { return anyPicture() || anyLook() || reset; }
  bool anyPicture() const { return has_size || has_quality || has_fps; }
  bool anyLook() const {
    return has_bright || has_contrast || has_low || has_vflip || has_hmirror || has_wb || has_night;
  }
};

struct WsCommand {
  WsCmdKind kind = WsCmdKind::kInvalid;
  bool has_id = false;
  int32_t id = 0;
  int8_t v = 0;  // drive: forward +
  int8_t w = 0;  // drive: turn left + (page sends w = -x)
  int8_t x = 0;  // cam_joy: pan right +
  int8_t y = 0;  // cam_joy: tilt up +
  bool active = false;
  uint8_t sound_id = 0;  // sound: proto::SoundId value (1..23)
  bool on = false;       // cam_lock
  uint8_t audio_mode = 0;  // audio_mode: proto::AudioModeValue (0 normal, 1 quiet, 2 mute)
  int32_t turn_by_cdeg = 0;   // turn by: ±100..72000 (0.01°); ±720° does not fit 16 bits
  bool turn_has_to = false;   // turn to an absolute heading instead of by
  uint16_t turn_to_cdeg = 0;  // turn to: 0..35999 (0.01°)
  uint8_t speed = 40;         // turn: 12..100 %, move: 10..100 % (checked per command)
  uint16_t radius_cm = 0;     // turn: 0 = spin in place (> 0 needs encoders)
  bool pivot = false;         // turn: pivot around the inside track (Plan L)
  int16_t rate_ddps = 0;      // drive_rate: -1800..1800 (0.1 °/s, + = CCW / left)
  uint8_t debug_hz = 10;      // debug: 10 or 50
  char ssid[33] = "";      // wifi_save (Plan M)
  char password[64] = "";  // wifi_save / ap_password - never logged or echoed
  int8_t slot = -1;        // wifi_save / wifi_forget: 0..2, -1 = first free / all
  bool wifi_ap = false;    // wifi_mode: true = own AP
  uint8_t setting_id = 0;  // setting
  bool has_value = false;
  int32_t value = 0;
  bool value_bool = false;  // value was true / false (1 / 0): skill_answer only
  bool try_only = false;  // setting (R23): SETTING_SET alone, not saved - the camera trims only
  int8_t calib_reset = -1;  // calib: 0 counts_per_m, 1 roll, 2 gyro_scale (R19)
  int16_t move_cm = 0;        // move: ±1..500 (+ forward)
  bool has_cpm = false;       // calib: set (else read)
  uint16_t cpm = 0;           // calib: counts per metre
  bool has_gyro_scale = false;  // R19 calib: set the gyro scale
  uint16_t gyro_scale = 0;      // R19: parts per 10 000 (10230 = x1.0230)
  uint32_t log_since = 0;     // log (Plan O)
  CamFields cam;              // camera_set
  int16_t cam_pan_dd = 900;   // cam_set (R22): the Uno's servo values, 900 - deg x 10 ...
  int16_t cam_tilt_dd = 900;
  bool cam_in_range = false;  // ... and pan -90..90 / tilt -20..60 (else nack OUT_OF_RANGE)
  uint32_t prog = 0;          // logo_run: the number the check returned
  bool step = false;          // logo_run: step mode
  bool has_things = false;    // hello / skill_things carried a "things" key (its list: parseThingList)
  logo::SkillEnd skill_end = static_cast<logo::SkillEnd>(0);  // skill_failed: the reason (kNotFound..kStale)
  uint8_t part = 0, of = 0;   // skill_things: part i of n (1 <= part <= of <= 48)
  char thing[25] = "";        // skill_failed: the thing's word (optional)
  char t0[24] = "";  // ping: number echoed verbatim in pong
  char client[24] = "";
  uint16_t page_port = 0;  // hello (Plan Z2b-1, R27.2): "page":{"port","proto"} - where the engine's AI feed is; 0 = none
  uint8_t page_proto = 0;
};

WsCommand parseWsCommand(const char* text, size_t len);
// hello / skill_things carry their list apart from the flat command: core/thing_list.h parseThingList(text, len, out).

}  // namespace core
}  // namespace cq
