// WebSocket JSON in -> WsCommand (bridge LLD R2.9, spec 01 §4-5 names). Messages are small flat
// objects; this is a fixed-buffer reader for exactly that (no JSON library, no heap). Unknown keys
// (e.g. a gateway "key") are skipped, nested values are skipped, numbers are clamped to -100..100.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
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
  kUnsupported,  // valid JSON, "t" not in the Basic Drive slice -> nack INVALID_COMMAND
  kInvalid,      // not JSON / no "t" / bad field -> nack INVALID_COMMAND
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
  uint8_t speed = 40;         // turn: 10..100 %
  uint16_t radius_cm = 0;     // turn: 0 = spin in place (> 0 needs encoders)
  int16_t rate_ddps = 0;      // drive_rate: -1800..1800 (0.1 °/s, + = CCW / left)
  uint8_t debug_hz = 10;      // debug: 10 or 50
  char t0[24] = "";  // ping: number echoed verbatim in pong
  char client[24] = "";
};

WsCommand parseWsCommand(const char* text, size_t len);

}  // namespace core
}  // namespace cq
