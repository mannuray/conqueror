#include "app/app.h"
#include "app/camera_api.h"
#include "app/tasks.h"
#include "ports/clock.h"
#include "ports/servos.h"

namespace cq {
namespace app {

void configureCamera() {
  const GlobalSettings& gs = g_app.settings.global();
  g_app.camera.configure(CameraConfig{gs.pan_min, gs.pan_max, gs.tilt_min, gs.tilt_max, gs.pan_trim, gs.tilt_trim,
                                      gs.servo_speed, gs.servo_accel, gs.cam_return_delay, gs.servo_idle_detach != 0});
}

void cameraInit() {
  port::servos::begin();
  port::servos::writeDeg(Axis::kPan, g_app.camera.panOutDeg());
  port::servos::writeDeg(Axis::kTilt, g_app.camera.tiltOutDeg());
  port::servos::attach(Axis::kPan);
  port::servos::attach(Axis::kTilt);
}

void cameraTask() {
  g_app.camera.tick(0.02f, port::clock::nowMs());
  if (g_app.camera.wantAttached()) {
    port::servos::attach(Axis::kPan);
    port::servos::attach(Axis::kTilt);
    port::servos::writeDeg(Axis::kPan, g_app.camera.panOutDeg());
    port::servos::writeDeg(Axis::kTilt, g_app.camera.tiltOutDeg());
  } else {
    port::servos::detach(Axis::kPan);
    port::servos::detach(Axis::kTilt);
  }
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(proto::FastFlagBit::kCamMoving));
  g_app.fast_flags = g_app.camera.moving() ? static_cast<uint16_t>(g_app.fast_flags | m)
                                           : static_cast<uint16_t>(g_app.fast_flags & ~m);
  g_app.fast.pan = static_cast<int16_t>(g_app.camera.panDeg() * 10.0f);
  g_app.fast.tilt = static_cast<int16_t>(g_app.camera.tiltDeg() * 10.0f);
}

proto::AckStatus onCamJoy(const uint8_t* p, uint8_t len) {
  proto::CamJoyMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.joy(m.x, m.y, m.active != 0, port::clock::nowMs());
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamSet(const uint8_t* p, uint8_t len) {
  proto::CamSetMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.set(m.pan * 0.1f, m.tilt * 0.1f, m.speed * 2.0f);
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamNudge(const uint8_t* p, uint8_t len) {
  proto::CamNudgeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.nudge(m.dpan * 0.1f, m.dtilt * 0.1f);
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamHome(const uint8_t* p, uint8_t len) {
  proto::CamHomeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.home();
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamLock(const uint8_t* p, uint8_t len) {
  proto::CamLockMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.lock(m.on != 0);
  return proto::AckStatus::kOk;
}

}  // namespace app
}  // namespace cq
