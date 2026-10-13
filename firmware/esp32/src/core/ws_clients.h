// WebSocket client table (bridge LLD R2.6 WsClient): up to 4 clients named phone-1, phone-2, ...;
// per-client rate limits (drive and cam_joy 25 per second, everything 60 per second; STOP is never
// limited); clients silent for 3 s are reaped (the page pings every 1 s); a streaming command
// rejection is reported once per reason, not 20 times a second. Pure C++.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/ws_codec.h"

namespace cq {
namespace core {

constexpr size_t kMaxWsClients = 4;
constexpr uint32_t kWsSilentMs = 3000;
constexpr uint8_t kStreamLimitPerS = 25;
constexpr uint8_t kTotalLimitPerS = 60;

class WsClients {
 public:
  bool add(int fd, uint32_t now_ms);  // false: table full (caller closes the socket)
  bool remove(int fd);                // false: not a WS client (plain HTTP socket)
  bool contains(int fd) const { return find(fd) >= 0; }
  size_t count() const { return m_count; }
  int fdAt(size_t i) const { return m_c[i].fd; }  // i < count()
  const char* name(int fd) const;                 // "" when unknown
  // Counts the message; false = over the limit (drop it, nack RATE_LIMITED). STOP always passes.
  bool admit(int fd, WsCmdKind kind, uint32_t now_ms);
  // Up to max silent clients into out; returns how many.
  size_t silent(uint32_t now_ms, int* out, size_t max) const;
  // Streaming rejections: true when this reason should be reported (first time in a row).
  bool reportStreamReject(int fd, uint8_t reason);
  void streamAccepted(int fd);

 private:
  struct Client {
    int fd;
    char name[12];
    uint32_t last_rx_ms;
    uint32_t window_ms;
    uint8_t stream_n;
    uint8_t cam_n;
    uint8_t total_n;
    uint8_t last_reject;  // 0xFF = none
  };
  int find(int fd) const;
  Client m_c[kMaxWsClients] = {};
  size_t m_count = 0;
  uint32_t m_next_number = 1;
};

// STOP fast path on link_q (bridge LLD R2.11): the WS handler stamps every text message with a
// post number and puts `stop` at the queue front. A drive / cam_joy posted before the STOP but
// handled after it is stale and dropped silently - otherwise it would restart the tank. Global,
// not per client: a STOP from any phone also cancels the driver's older drives.
class StopBarrier {
 public:
  void onStop(uint32_t post_seq) {
    if (!m_set || static_cast<int32_t>(post_seq - m_seq) > 0) m_seq = post_seq;
    m_set = true;
  }
  bool stale(uint32_t post_seq) {  // counts the drops
    const bool s = m_set && static_cast<int32_t>(post_seq - m_seq) < 0;
    if (s) ++m_dropped;
    return s;
  }
  uint32_t dropped() const { return m_dropped; }

 private:
  uint32_t m_seq = 0;
  bool m_set = false;
  uint32_t m_dropped = 0;
};

}  // namespace core
}  // namespace cq
