// Who drives and the safety path: drive, STOP (+ the hold, design-notes §72), release, camera, client open / close,
// silent-client reaping, owner changes from the Uno, OTA (bridge LLD R2.7, R14, R15.4). Owns the STOP barrier.
#pragma once

#include <stdint.h>

#include "app/motion_controller.h"
#include "app/replies.h"
#include "app/settings_controller.h"
#include "app/telemetry_publisher.h"
#include "app/uno_channel.h"
#include "core/event_log.h"
#include "core/link_owner.h"
#include "core/ws_clients.h"
#include "core/ws_codec.h"

namespace cq {
namespace app {

class DriveController {
 public:
  DriveController(Replies& out, core::EventLog& log, core::LinkOwner& owner, UnoChannel& uno, MotionController& motion,
                  SettingsController& settings, TelemetryPublisher& tel)
      : m_out(out), m_log(log), m_owner(owner), m_uno(uno), m_motion(motion), m_settings(settings), m_tel(tel) {}
  void begin(uint32_t now);  // R14: master of the Uno link from boot
  core::Decision claim(int fd, uint32_t now);  // take / keep ownership for a motion command
  void applyOwner(const core::Actions& a, uint32_t now);
  core::OwnerView ownerView() const;
  const core::LinkOwner& owner() const { return m_owner; }
  bool otaActive() const { return m_ota; }
  // False when the command is dropped (stale behind a STOP) or rate limited (answered).
  bool admitted(int fd, core::WsCmdKind kind, int32_t id, uint32_t post_seq, uint32_t now);
  void onStop(int fd, int32_t id, uint32_t post_seq, uint32_t now);  // fast path: may overtake its own open
  void onDrive(int fd, const core::WsCommand& c, uint32_t now);
  void onCamJoy(int fd, const core::WsCommand& c);
  void onUnoRequest(int fd, int32_t id, const core::WsCommand& c);  // cam_home / cam_lock / sound / audio_mode
  void onCamSet(int fd, int32_t id, const core::WsCommand& c);      // R22: the camera to exact angles
  void onRelease(int fd, int32_t id, uint32_t now);
  void onDebug(int fd, int32_t id, const core::WsCommand& c);
  void onClientOpen(int fd, uint32_t now);
  void onClientClose(int fd, uint32_t now);
  void onUnoOwner(uint8_t owner, uint32_t now);
  void onSynced(bool synced, uint32_t now);
  void startOta(uint32_t now);
  void tick(uint32_t now);  // a new slow frame's owner, the 5 s handback, silent clients

 private:
  void reapSilent(uint32_t now);
  bool requestRefused(const core::WsCommand& c, core::Decision d, core::BridgeReason& why) const;

  Replies& m_out;
  core::EventLog& m_log;
  core::LinkOwner& m_owner;
  UnoChannel& m_uno;
  MotionController& m_motion;
  SettingsController& m_settings;
  TelemetryPublisher& m_tel;
  core::StopBarrier m_barrier;  // STOP fast path: drop motion posted before a STOP
  bool m_ota = false;           // from now on: drives nacked BUSY, new phones refused
  uint32_t m_last_slow = 0;
};

}  // namespace app
}  // namespace cq
