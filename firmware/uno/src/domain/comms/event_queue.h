#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct Event {
  uint8_t kind;
  int16_t a;
  int16_t b;
};

class EventQueue {
 public:
  bool push(proto::EventKind kind, int16_t a = 0, int16_t b = 0);
  bool peek(Event& out) const;  // oldest event, not removed
  bool pop(Event& out);
  bool empty() const { return m_count == 0; }
  uint16_t dropped() const { return m_dropped; }

 private:
  static constexpr uint8_t kSize = 8;
  Event m_ring[kSize] = {};
  uint8_t m_head = 0;
  uint8_t m_count = 0;
  uint16_t m_dropped = 0;
};

class AckQueue {
 public:
  bool push(uint8_t seq, proto::AckStatus status);
  bool peek(uint8_t& seq, proto::AckStatus& status) const;  // oldest ACK, not removed
  bool pop(uint8_t& seq, proto::AckStatus& status);

 private:
  static constexpr uint8_t kSize = 4;
  uint8_t m_seq[kSize] = {};
  proto::AckStatus m_status[kSize] = {};
  uint8_t m_head = 0;
  uint8_t m_count = 0;
};

// At most one rejection event per 500 ms per source (0 none, 1 human, 2 ai).
class RejectLimiter {
 public:
  bool allow(uint8_t source, uint32_t now_ms);

 private:
  uint32_t m_last_ms[3] = {};
  bool m_seen[3] = {};
};

}  // namespace cq
