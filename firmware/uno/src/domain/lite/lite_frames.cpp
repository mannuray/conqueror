#include "domain/lite/lite_frames.h"

namespace cq {
namespace lite_frames {

using F = proto::FrameType;

FrameUse frameUse(uint8_t type) {
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: case F::kHelloAck: case F::kHelloReq: case F::kEspAck:
    case F::kDrive: case F::kStop: case F::kBrake: case F::kEnvelope: case F::kOwnerHint:
    case F::kCamJoy: case F::kCamSet: case F::kCamNudge: case F::kCamHome: case F::kCamLock:
    case F::kSettingSet: case F::kSettingGet: case F::kSettingsSave:
      return FrameUse::kHandled;
    case F::kDriveRate: case F::kTurn: case F::kMove: case F::kCancel: case F::kCalibrate: case F::kProfile:
    case F::kSafetyOverride: case F::kLed: case F::kSound: case F::kTone: case F::kAudioMode:
      return FrameUse::kUnavailable;
    default:
      return FrameUse::kUnknown;
  }
}

bool needsAck(uint8_t type) {
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: case F::kHelloAck: case F::kHelloReq: case F::kEspAck:
    case F::kDrive: case F::kOwnerHint: case F::kCamJoy:
      return false;
    default:
      return frameUse(type) != FrameUse::kUnknown;
  }
}

uint8_t helloCaps(bool encoders_enabled) {
  return encoders_enabled ? static_cast<uint8_t>(1u << static_cast<uint8_t>(proto::CapBit::kEncoders)) : 0;
}

}  // namespace lite_frames
}  // namespace cq
