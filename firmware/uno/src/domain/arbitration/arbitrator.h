#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Motion ownership (LLD §9.2): IR > calibration > network. Link loss stops only network/calibration
// motion; the deadman applies only to streamed network teleop (DRIVE/DRIVE_RATE), never primitives.
// Before sync (HELLO_ACK) network motion and calibration get BUSY; IR works (LLD §9.4). Network → None
// after 1 s without a network motion command while no primitive runs (LLD §9.18).
class Arbitrator {
 public:
  enum class Action : uint8_t { kNone, kDeadmanStop, kLinkLostStop, kIrReleased };
  struct Config {
    uint16_t deadman_ms;
    uint16_t ir_handback_ms;
  };
  static constexpr uint16_t kNetworkIdleMs = 1000;

  void setSynced(bool synced) { m_synced = synced; }
  proto::AckStatus requestNetwork(bool teleop, uint32_t now_ms);
  void onNetworkStop();
  bool onIrInput(uint32_t now_ms);
  void releaseIr() {
    if (m_owner == proto::Owner::kIr) setOwner(proto::Owner::kNone);
  }
  proto::AckStatus beginCalibration();
  void endCalibration();
  Action tick(uint32_t now_ms, bool link_alive, bool primitive_busy, const Config& cfg);
  proto::Owner owner() const { return m_owner; }
  bool takeOwnerChanged() {
    const bool c = m_changed;
    m_changed = false;
    return c;
  }

 private:
  void setOwner(proto::Owner o) {
    if (o != m_owner) m_changed = true;
    m_owner = o;
  }
  proto::Owner m_owner = proto::Owner::kNone;
  bool m_changed = false;
  bool m_synced = false;
  bool m_teleop = false;
  uint32_t m_last_teleop_ms = 0;
  uint32_t m_last_network_ms = 0;
  uint32_t m_last_ir_ms = 0;
};

}  // namespace cq
