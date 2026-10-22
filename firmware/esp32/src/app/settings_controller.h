// WiFi, calibration and settings changes, the tank's button, restart (bridge LLD R9, R13, R15.4).
// A change from a page waits for one press of the tank's button (ConfirmGate); a double press switches the WiFi.
#pragma once

#include <stdint.h>

#include "app/replies.h"
#include "core/acked_ops.h"
#include "core/body_link.h"
#include "core/confirm.h"
#include "core/event_log.h"
#include "core/wifi_types.h"
#include "core/ws_codec.h"

namespace cq {
namespace app {

class SettingsController {
 public:
  struct Hooks {  // until MotionController / UnoChannel / DriveController own these (Plan P Tasks 9-11)
    bool (*motion_active)();
    bool (*ota)();
    int (*driver_fd)();
    void (*send_calib)(int fd);
    bool (*queue_op)(const core::AckedOp& op);  // false: the Uno request queue is full
    uint16_t* cpm;      // counts per metre in use
    uint16_t* roll_mm;  // learned roll-on in use
    size_t (*free_slots)();  // free Uno request slots (shared with the phones' requests, R15.5)
    uint16_t* coast_ms = nullptr;  // R18.2: learned turn coast in use (reset with the roll-on)
    bool (*set_gyro_scale)(uint16_t) = nullptr;  // R19: hand a saved gyro scale to the heading correction
  };

  SettingsController(Replies& out, core::EventLog& log, const core::BodyLink& link)
      : m_out(out), m_log(log), m_link(link) {}
  void setHooks(const Hooks& h) { m_h = h; }
  void onCommand(int fd, int32_t id, const core::WsCommand& c, uint32_t now);  // wifi_*, ap_password, setting, restart
  void onCalib(int fd, int32_t id, const core::WsCommand& c, uint32_t now);    // read / set / reset
  void onButtonPress(uint32_t now);
  void onClientClosed(int fd) { m_gate.dropClient(fd); }
  void tick(uint32_t now);  // expired confirmations, WiFi results, a due restart

 private:
  uint32_t tankMs() const;
  void askConfirm(const core::PendingChange& c, uint32_t now);
  void sendWifiStatus(int fd);
  bool wifiIsAp();
  void pushFeedback(bool to_ap);
  void onSetting(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onRestart(int fd, int32_t id, uint32_t now);
  void onScan(int fd, int32_t id);
  void applyChange(core::PendingChange& ch);
  void applyWifi(core::PendingChange& ch);
  void applyCalibSet(const core::PendingChange& ch);
  void applyCalibReset(const core::PendingChange& ch);
  void applyGyroScale(const core::PendingChange& ch, uint16_t scale_e4);
  void applySetting(const core::PendingChange& ch);
  void onWifiResult(const core::WifiResult& res, uint32_t now);

  Replies& m_out;
  core::EventLog& m_log;
  const core::BodyLink& m_link;
  Hooks m_h{};
  core::ConfirmGate m_gate;
  core::ButtonRouter m_button{m_gate};
  uint32_t m_restart_at = 0;
};

}  // namespace app
}  // namespace cq
