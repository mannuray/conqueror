// Frame boundaries in the passthrough byte streams (bridge LLD R2.6/R2.8). Never changes a byte:
// complete frames (CRC good or bad - passthrough stays transparent) and runs of bytes outside any
// frame are handed to the sink as whole chunks; a partial frame waits for its remaining bytes and
// is flushed raw after 50 ms (the Uno parser resyncs) or discarded when the client closes.
// Sink: void chunk(const uint8_t* data, size_t len, ChunkKind kind). Pure C++, no heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr uint32_t kPartialFlushMs = 50;
constexpr size_t kRawRunMax = 64;

enum class ChunkKind : uint8_t { kFrame, kBadFrame, kRaw };

class FrameAligner {
 public:
  template <class Sink>
  void push(const uint8_t* data, size_t len, uint32_t now_ms, Sink& sink) {
    for (size_t i = 0; i < len; ++i) pushByte(data[i], now_ms, sink);
    flushRaw(sink);  // bytes outside frames go out at once
  }

  template <class Sink>
  void flushIfStale(uint32_t now_ms, Sink& sink) {
    if (m_n != 0 && now_ms - m_started_ms >= kPartialFlushMs) {
      sink.chunk(m_buf, m_n, ChunkKind::kRaw);
      m_n = 0;
      ++m_flushed;
    }
  }

  void discardPartial() { m_n = 0; }
  void reset() {
    m_n = 0;
    m_raw_n = 0;
    m_have_seq = false;
  }
  bool midFrame() const { return m_n != 0; }
  bool haveSeq() const { return m_have_seq; }
  uint8_t lastSeq() const { return m_last_seq; }  // seq of the last complete frame
  uint32_t frames() const { return m_frames; }
  uint32_t badCrc() const { return m_bad_crc; }
  uint32_t flushed() const { return m_flushed; }

 private:
  template <class Sink>
  void pushByte(uint8_t b, uint32_t now_ms, Sink& sink) {
    if (m_n == 0) {
      if (b == proto::kSync) {
        m_buf[0] = b;
        m_n = 1;
        m_started_ms = now_ms;
      } else {
        rawByte(b, sink);
      }
      return;
    }
    if (m_n == 1 && b > proto::kMaxPayload) {  // the SYNC did not start a frame
      m_n = 0;
      rawByte(proto::kSync, sink);
      pushByte(b, now_ms, sink);  // b may itself be a SYNC
      return;
    }
    m_buf[m_n++] = b;
    if (m_n >= 2 && m_n == static_cast<size_t>(m_buf[1]) + 5u) {
      const uint8_t crc = proto::crc8(m_buf + 1, static_cast<uint8_t>(m_buf[1] + 3));
      const bool good = crc == m_buf[m_n - 1];
      flushRaw(sink);
      sink.chunk(m_buf, m_n, good ? ChunkKind::kFrame : ChunkKind::kBadFrame);
      if (good) {
        ++m_frames;
        m_last_seq = m_buf[3];
        m_have_seq = true;
      } else {
        ++m_bad_crc;
      }
      m_n = 0;
    }
  }

  template <class Sink>
  void rawByte(uint8_t b, Sink& sink) {
    m_raw[m_raw_n++] = b;
    if (m_raw_n == kRawRunMax) flushRaw(sink);
  }

  template <class Sink>
  void flushRaw(Sink& sink) {
    if (m_raw_n == 0) return;
    sink.chunk(m_raw, m_raw_n, ChunkKind::kRaw);
    m_raw_n = 0;
  }

  uint8_t m_buf[proto::kMaxPayload + 5] = {};
  size_t m_n = 0;
  uint32_t m_started_ms = 0;
  uint8_t m_raw[kRawRunMax] = {};
  size_t m_raw_n = 0;
  uint8_t m_last_seq = 0;
  bool m_have_seq = false;
  uint32_t m_frames = 0;
  uint32_t m_bad_crc = 0;
  uint32_t m_flushed = 0;
};

// Drops the Uno's ACK(s) for a STOP the bridge injected during passthrough, so a stray ACK never
// resolves a bench command (LLD R2.8 step 3). Resends produce one ACK each; all are dropped
// until the deadline.
class AckSwallow {
 public:
  void arm(uint8_t seq, uint32_t until_ms) {
    m_seq = seq;
    m_until_ms = until_ms;
    m_armed = true;
  }
  bool shouldDrop(const uint8_t* frame, size_t len, uint32_t now_ms) {
    if (!m_armed) return false;
    if (static_cast<int32_t>(now_ms - m_until_ms) >= 0) {
      m_armed = false;
      return false;
    }
    if (len == 7 && frame[2] == static_cast<uint8_t>(proto::FrameType::kAck) &&
        frame[4] == m_seq) {
      ++m_dropped;
      return true;
    }
    return false;
  }
  uint32_t dropped() const { return m_dropped; }

 private:
  uint8_t m_seq = 0;
  uint32_t m_until_ms = 0;
  bool m_armed = false;
  uint32_t m_dropped = 0;
};

}  // namespace core
}  // namespace cq
