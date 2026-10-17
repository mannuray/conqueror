// Plan H review I1 / I2 (bridge LLD R4.6): the decisions between a running primitive, the link owner and the
// frames to the body, in one host-tested place. The link task only executes a LinkOut.
// Rules: a primitive never takes ownership (LinkOwner::keepDriving); while running it sends DRIVE src AUTO;
// it ends with a STOP only while its client still owns the body - never after IR / another client / the bench
// took over (a bridge STOP would stop them), and not when a STOP is already on its way.
#pragma once

#include <stdint.h>

#include "core/link_owner.h"
#include "core/motion_service.h"

namespace cq {
namespace core {

// Things that end a running primitive from outside the service.
enum class MotionEvent : uint8_t {
  kStop,          // STOP from anyone (the STOP fast path already sends it)
  kRelease,       // the owner released control (LinkOwner already sent STOP)
  kOwnerClosed,   // the owner's WebSocket closed (LinkOwner sends STOP for its driver)
  kOta,           // OTA starts (link task sends STOP)
  kBodyOffline,   // the Uno is gone
  kOwnerCommand,  // the owner's own drive / turn / drive_rate replaces it (its frames follow)
  kCancel,        // the owner's cancel: STOP from here
  kTxFull,        // a DRIVE could not be queued while the body is still synced: end "link" with a STOP
};

struct LinkOut {
  bool drive = false;  // send DRIVE {src AUTO, v, w}
  int8_t v = 0;
  int8_t w = 0;
  bool stop = false;   // send STOP
  bool event = false;  // broadcast primitive_done{end}
  PrimitiveEnd end{};
};

class MotionLink {
 public:
  explicit MotionLink(LinkOwner& owner) : m_owner(owner) {}
  MotionService& service() { return m_svc; }
  const MotionService& service() const { return m_svc; }
  LinkOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  LinkOut tick(uint32_t now_ms);
  LinkOut onEvent(MotionEvent e, uint32_t now_ms);

 private:
  LinkOut finished(const MotionOut& o, int fd, uint32_t now_ms);
  LinkOwner& m_owner;
  MotionService m_svc;
};

}  // namespace core
}  // namespace cq
