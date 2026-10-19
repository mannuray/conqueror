// Every frame to the Uno that waits for its answer, in one queue (bridge LLD R15.5). STOP keeps BodyLink's urgent
// slot (resent until the Uno's ACK); its waiters are answered here. One request in flight at a time.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/replies.h"
#include "core/acked_ops.h"
#include "core/body_link.h"
#include "core/frame_tx.h"

namespace cq {
namespace app {

class UnoChannel {
 public:
  UnoChannel(core::BodyLink& link, core::FrameTx& tx, Replies& out) : m_link(link), m_tx(tx), m_out(out) {}
  bool request(const core::AckedOp& op) { return m_ops.push(op); }  // false: 8 queued - the caller nacks BUSY
  void waitStop(int fd, int32_t id);  // the STOP already went (urgent slot): answer fd when the Uno acks it
  void onAck(uint8_t seq, uint8_t status);
  void onStopResult(uint8_t status);
  void onSettingValue(uint8_t id, int32_t value);
  void onBodyOffline();  // every queued / in-flight request with a client: BODY_OFFLINE, dropped
  void tick(uint32_t now);  // send the next request; the 600 ms backstop (BodyLink's 200 ms timeout comes as onAck)
  void stopForOta();        // R14: the link stops only for OTA; every waiter answered
  core::BodyLink& link() { return m_link; }
  const core::BodyLink& link() const { return m_link; }
  bool empty() const { return m_ops.empty(); }

 private:
  struct Waiter {
    int fd;
    int32_t id;
  };
  static constexpr size_t kMaxStopWaiters = 2 * core::kMaxWsClients;  // review M8

  bool send(const core::AckedOp& op, uint32_t now, uint8_t& seq);
  void backstop(uint32_t now);
  void answerAll(core::BridgeReason r);  // the in-flight request and every queued one, then empty

  core::BodyLink& m_link;
  core::FrameTx& m_tx;
  Replies& m_out;
  core::AckedOps m_ops;
  Waiter m_stop_waiters[kMaxStopWaiters] = {};
  size_t m_stop_n = 0;
};

}  // namespace app
}  // namespace cq
