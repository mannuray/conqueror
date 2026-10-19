// Who is the body-link master and who drives (bridge LLD R2.7, owner-accepted R2.14 #1/#2).
// The ESP32 is the master while a web client is connected (the bench TCP passthrough was removed, LLD R10).
// A phone owns driving from its first drive until release_control, 5 s without drive, or its socket closes. STOP from anyone in every state.
// IR on the Uno beats all. Pure C++: returns decisions and actions, the caller executes them.
#pragma once

#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr uint32_t kHandbackMs = 5000;  // gateway human_handback_ms

enum class LinkState : uint8_t { kIdle, kWebHandshake, kWebReady, kWebDriving };
enum class Decision : uint8_t { kSend, kNackNotOwner, kNackBusy, kNackBodyOffline, kNackStopped };

// Things the caller must do, in this order: kStopMaster, kSendStop, kHintNone /
// kHintHuman, kStartMaster, kOwnerChanged (broadcast ev control_changed + ts).
struct Actions {
  enum : uint8_t {
    kStartMaster = 1 << 0,
    kStopMaster = 1 << 1,
    kSendStop = 1 << 2,  // STOP with BodyLink's own seq (resent until the Uno's ACK)
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

  Actions begin();  // R14 (design-notes §95): the bridge is the Uno link's only master, from boot
  Actions onWsOpen(int fd);
  Actions onWsClose(int fd);
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
