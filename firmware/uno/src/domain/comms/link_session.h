#pragma once
#include <stdint.h>

namespace cq {

enum class LinkChange : uint8_t { kNone, kLost, kRestored };

// Link liveness (300 ms), HELLO handshake (every 500 ms until HELLO_ACK) and one
// Uno-originated request (WIFI/ADMIN reset) resent every 500 ms until ESP_ACK.
class LinkSession {
 public:
  static constexpr uint16_t kLinkTimeoutMs = 300;
  static constexpr uint16_t kRepeatMs = 500;

  // Link watch (every 10 ms): reports alive/dead edges and runs onLinkLost() on a loss.
  // The first contact after boot is not a restore: LINK_RESTORED only follows a LINK_LOST.
  LinkChange poll(uint32_t now_ms);

  void onValidFrame(uint32_t now_ms) {
    m_last_rx_ms = now_ms;
    m_seen = true;
  }
  bool alive(uint32_t now_ms) const;
  void onLinkLost() {
    m_synced = false;
    m_hello_due = true;
  }
  void onHelloAck() { m_synced = true; }
  void onHelloReq() { m_hello_due = true; }
  bool synced() const { return m_synced; }
  bool helloDue(uint32_t now_ms);

  void setRequest(uint8_t frame_type) {
    m_req_type = frame_type;
    m_req_sent = false;
  }
  bool requestDue(uint32_t now_ms, uint8_t& frame_type);
  void onRequestSent(uint8_t seq, uint32_t now_ms) {
    m_req_seq = seq;
    m_req_last_ms = now_ms;
    m_req_sent = true;
  }
  void onEspAck(uint8_t ack_seq) {
    if (m_req_type != 0 && m_req_sent && ack_seq == m_req_seq) m_req_type = 0;
  }

 private:
  uint32_t m_last_rx_ms = 0;
  bool m_seen = false;
  bool m_was_alive = false;   // alive at the previous poll()
  bool m_ever_alive = false;  // any contact since boot
  bool m_synced = false;
  bool m_hello_due = true;
  uint32_t m_last_hello_ms = 0;
  uint8_t m_req_type = 0;
  uint8_t m_req_seq = 0;
  bool m_req_sent = false;
  uint32_t m_req_last_ms = 0;
};

}  // namespace cq
