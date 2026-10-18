// Who is the body-link master and who drives (bridge LLD R2.7, owner-accepted R2.14 #1/#2).
// Bench owns the link from TCP connect. A phone owns driving from its first drive until
// release_control, 5 s without drive, or its socket closes. STOP from anyone in every state.
// IR on the Uno beats all. Pure C++: returns decisions and actions, the caller executes them.
#pragma once

#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr uint32_t kHandbackMs = 5000;  // gateway human_handback_ms

enum class LinkState : uint8_t { kIdle, kWebHandshake, kWebReady, kWebDriving, kTcpPassthrough };
enum class Admit : uint8_t { kAccept, kRefuseBusy };
enum class Decision : uint8_t { kSend, kNackNotOwner, kNackBusy, kNackBodyOffline, kNackStopped };

// Things the caller must do, in this order: kStopMaster, kInjectStop / kSendStop, kHintNone /
// kHintHuman, kStartMaster, kOwnerChanged (broadcast ev control_changed + ts).
struct Actions {
  enum : uint8_t {
    kStartMaster = 1 << 0,
    kStopMaster = 1 << 1,
    kSendStop = 1 << 2,    // ESP32 is (or was) master: STOP with BodyLink's own seq
    kInjectStop = 1 << 3,  // passthrough: STOP with seq = lastSeq + 128, ACK swallowed
    kHintNone = 1 << 4,
    kHintHuman = 1 << 5,
    kOwnerChanged = 1 << 6,
  };
  uint8_t bits = 0;
  bool has(uint8_t a) const { return (bits & a) != 0; }
};

class LinkOwner {
 public:
  static constexpr int kNoFd = -1;

  Actions onWsOpen(int fd);
  Actions onWsClose(int fd);
  Admit onTcpConnect(Actions& out);
  Actions onTcpClose();
  // moving: a drive frame with v or w != 0. After a STOP from another device the driver's moving frames are refused
  // (kNackStopped) until it releases its stick (a 0/0 frame), its own STOP, or a new primitive (moving = false).
  Decision onDrive(int fd, uint32_t now_ms, Actions& out, bool moving = false);
  Decision onCamera(int fd) const;  // cam_joy / cam_home: no ownership taken
  // Plan H review I1: a running primitive keeps its owner alive (handback timer) but never takes ownership -
  // Send only while `fd` is the web driver; otherwise the primitive ends.
  Decision keepDriving(int fd, uint32_t now_ms);
  Actions onStop(int fd);
  Actions onRelease(int fd);
  Actions onSynced(bool synced);
  Actions onUnoOwner(proto::Owner owner);  // from TELEM_SLOW.owner or EVENT control_changed
  Actions tick(uint32_t now_ms);

  LinkState state() const { return m_state; }
  int driverFd() const { return m_driver; }
  uint8_t wsCount() const { return m_ws; }
  bool irActive() const { return m_ir; }
  bool otaAllowed() const {
    return m_state == LinkState::kIdle || m_state == LinkState::kWebHandshake ||
           m_state == LinkState::kWebReady;
  }

 private:
  Actions dropDriver(bool send_stop);
  bool webMaster() const {
    return m_state == LinkState::kWebHandshake || m_state == LinkState::kWebReady ||
           m_state == LinkState::kWebDriving;
  }

  LinkState m_state = LinkState::kIdle;
  int m_driver = kNoFd;
  bool m_stop_hold = false;  // design-notes §72: a STOP from another device holds until the stick is released
  uint8_t m_ws = 0;
  bool m_synced = false;
  bool m_ir = false;
  uint32_t m_last_drive_ms = 0;
};

}  // namespace core
}  // namespace cq
