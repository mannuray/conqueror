#include <avr/pgmspace.h>

#include "app/app.h"
#include "app/comms.h"
#include "ports/clock.h"

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

}  // namespace

// ROUTES-BEGIN (later tasks add rows before ROUTES-END)
const Route kRoutes[] PROGMEM = {
    {static_cast<uint8_t>(proto::FrameType::kHeartbeat), onHeartbeat, false},
    {static_cast<uint8_t>(proto::FrameType::kHelloAck), onHelloAck, false},
    {static_cast<uint8_t>(proto::FrameType::kHelloReq), onHelloReq, false},
    {static_cast<uint8_t>(proto::FrameType::kEspAck), onEspAck, false},
    // ROUTES-END
};
const uint8_t kRouteCount = sizeof kRoutes / sizeof kRoutes[0];

}  // namespace app
}  // namespace cq
