// The telemetry the pages see: td / tds (debug, per client), tf (10 Hz), ts (1 Hz), welcome (bridge LLD R2.14, R5, R15.4).
#pragma once

#include <stdint.h>

#include "app/replies.h"
#include "core/body_link.h"
#include "core/ws_json.h"

namespace cq {
namespace app {

class TelemetryPublisher {
 public:
  struct Views {  // filled by the root (DriveController / MotionController after Plan P Tasks 9-11)
    core::OwnerView (*owner)();
    core::PrimitiveView (*primitive)();
    bool (*body_offline)();
  };
  struct News {
    bool fast;
    bool slow;
  };

  TelemetryPublisher(Replies& out, const core::BodyLink& link) : m_out(out), m_link(link) {}
  void setViews(const Views& v) { m_views = v; }
  News poll();       // which TELEM frames are new since the last call; sends td / tds to the clients that asked
  void afterFast();  // after the motion step on a new TELEM_FAST: tf to everyone every 5th frame (Uno 50 Hz -> 10 Hz)
  void tickTs(uint32_t now);  // ts at 1 Hz, also while the Uno is silent
  void sendTs();              // ts now (owner changed)
  void hello(int fd, const char* fw, const char* sensor);  // welcome + ts to one client
  uint32_t nextEventSeq() { return ++m_ev_seq; }
  uint32_t tankMs() const;

 private:
  void sendDebug(bool fast, bool slow);

  Replies& m_out;
  const core::BodyLink& m_link;
  Views m_views{nullptr, nullptr, nullptr};
  uint32_t m_ev_seq = 0;
  uint32_t m_tf_seq = 0;
  uint32_t m_last_fast = 0;
  uint32_t m_last_slow = 0;
  uint32_t m_last_ts_ms = 0;
};

}  // namespace app
}  // namespace cq
