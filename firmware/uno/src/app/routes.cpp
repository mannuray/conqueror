#include <avr/pgmspace.h>

#include "app/app.h"
#include "app/comms.h"
#include "app/tasks.h"
#include "ports/clock.h"
#include "app/motion_api.h"

namespace cq {
namespace app {
namespace {

proto::AckStatus onHeartbeat(const uint8_t* p, uint8_t len) {
  proto::HeartbeatMsg m{};
  return proto::decode(p, len, m) ? proto::AckStatus::kOk : proto::AckStatus::kInvalidCommand;
}

proto::AckStatus onHelloAck(const uint8_t* p, uint8_t len) {
  proto::HelloAckMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.link.onHelloAck();
  return proto::AckStatus::kOk;
}

proto::AckStatus onHelloReq(const uint8_t* p, uint8_t len) {
  proto::HelloReqMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.link.onHelloReq();
  return proto::AckStatus::kOk;
}

proto::AckStatus onEspAck(const uint8_t* p, uint8_t len) {
  proto::EspAckMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.link.onEspAck(m.ack_seq);
  return proto::AckStatus::kOk;
}

proto::AckStatus onSettingSet(const uint8_t* p, uint8_t len) {
  proto::SettingSetMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.settings.set(m.id, m.value);
  if (st == proto::AckStatus::kOk) applySettings();
  return st;
}

proto::AckStatus onSettingGet(const uint8_t* p, uint8_t len) {
  proto::SettingGetMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  int32_t v = 0;
  if (!g_app.settings.get(m.id, v)) return proto::AckStatus::kSettingUnknown;
  g_app.setting_value.id = m.id;
  g_app.setting_value.value = v;
  g_app.setting_value_pending = true;
  return proto::AckStatus::kOk;
}

proto::AckStatus onSettingsSave(const uint8_t* p, uint8_t len) {
  proto::SettingsSaveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (g_app.saver.busy()) return proto::AckStatus::kSettingsSaveBusy;
  g_app.saver.request();
  return proto::AckStatus::kOk;
}

}  // namespace

// ROUTES-BEGIN (later tasks add rows before ROUTES-END)
const Route kRoutes[] PROGMEM = {
    {static_cast<uint8_t>(proto::FrameType::kHeartbeat), onHeartbeat, false},
    {static_cast<uint8_t>(proto::FrameType::kHelloAck), onHelloAck, false},
    {static_cast<uint8_t>(proto::FrameType::kHelloReq), onHelloReq, false},
    {static_cast<uint8_t>(proto::FrameType::kEspAck), onEspAck, false},
    {static_cast<uint8_t>(proto::FrameType::kSettingSet), onSettingSet, true},
    {static_cast<uint8_t>(proto::FrameType::kSettingGet), onSettingGet, true},
    {static_cast<uint8_t>(proto::FrameType::kSettingsSave), onSettingsSave, true},
    {static_cast<uint8_t>(proto::FrameType::kDrive), onDrive, false},
    {static_cast<uint8_t>(proto::FrameType::kStop), onStop, true},
    {static_cast<uint8_t>(proto::FrameType::kBrake), onBrake, true},
    {static_cast<uint8_t>(proto::FrameType::kEnvelope), onEnvelope, true},
    {static_cast<uint8_t>(proto::FrameType::kOwnerHint), onOwnerHint, false},
    {static_cast<uint8_t>(proto::FrameType::kSafetyOverride), onSafetyOverride, true},
    {static_cast<uint8_t>(proto::FrameType::kCalibrate), onCalibrate, true},
    {static_cast<uint8_t>(proto::FrameType::kDriveRate), onDriveRate, false},
    {static_cast<uint8_t>(proto::FrameType::kTurn), onTurn, true},
    {static_cast<uint8_t>(proto::FrameType::kMove), onMove, true},
    {static_cast<uint8_t>(proto::FrameType::kCancel), onCancel, true},
    {static_cast<uint8_t>(proto::FrameType::kProfile), onProfile, true},
    // ROUTES-END
};
const uint8_t kRouteCount = sizeof kRoutes / sizeof kRoutes[0];

}  // namespace app
}  // namespace cq
