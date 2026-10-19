// The bridge's measured motion: turn (spin / arc / pivot), move by distance, drive_rate, cancel, roll-on learning,
// counts per metre (bridge LLD R4, R7, R8, R15.4). Runs MotionLink and sends its DRIVE / STOP frames.
#pragma once

#include <stdint.h>

#include "app/replies.h"
#include "app/telemetry_publisher.h"
#include "core/body_link.h"
#include "core/event_log.h"
#include "core/link_owner.h"
#include "core/motion_link.h"
#include "core/roll_learner.h"
#include "core/ws_codec.h"

namespace cq {
namespace app {

class MotionController {
 public:
  struct Hooks {  // until DriveController owns these (Plan P Task 11)
    core::Decision (*claim)(int fd, uint32_t now);  // take / keep ownership (applies the owner's actions)
    bool (*ota)();
  };

  MotionController(Replies& out, core::EventLog& log, core::BodyLink& link, core::LinkOwner& owner,
                   TelemetryPublisher& tel)
      : m_out(out), m_log(log), m_link(link), m_tel(tel), m_motion(owner) {}
  void begin();  // counts per metre and roll-on from flash
  void setHooks(const Hooks& h) { m_h = h; }
  void onTurn(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onDriveRate(int fd, const core::WsCommand& c, uint32_t now);
  void onMove(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onCancel(int fd, int32_t id, uint32_t now);
  void onTelemetry(uint32_t now);  // a new TELEM_FAST: roll-on observation + the service step (50 Hz)
  void tick(uint32_t now);         // telemetry gap / drive_rate deadman
  void end(core::MotionEvent e, uint32_t now);
  bool ownedBy(int fd) const;
  bool active() const { return m_motion.service().active(); }
  void cancelRollOn() { m_roll.cancel(); }  // driving again / a new primitive
  uint16_t& cpm() { return m_cpm; }
  uint16_t& rollMm() { return m_roll_mm; }
  void sendCalib(int fd);
  core::PrimitiveView view() const;

 private:
  void run(const core::LinkOut& o, uint32_t now);
  bool turnAllowed(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  bool arcAllowed(int fd, int32_t id, const core::WsCommand& c);
  bool moveAllowed(int fd, int32_t id, uint32_t now);
  bool moveRefused(core::BridgeReason& why) const;
  bool rateRefused(int fd, uint32_t now, core::BridgeReason& why);
  bool claimed(int fd, int32_t id, uint32_t now);
  void observeRoll(uint32_t now);
  bool encoderFault() const;

  Replies& m_out;
  core::EventLog& m_log;
  core::BodyLink& m_link;
  TelemetryPublisher& m_tel;
  core::MotionLink m_motion;
  core::RollLearner m_roll;
  Hooks m_h{nullptr, nullptr};
  uint16_t m_cpm = 0;
  uint16_t m_roll_mm = 0;
  int32_t m_roll_last = -1;  // diagnostics in the calib reply: last reading (mm, -1 none) ...
  uint16_t m_roll_begun = 0;  // ... moves watched
  uint16_t m_roll_read = 0;   // ... readings taken
};

}  // namespace app
}  // namespace cq
