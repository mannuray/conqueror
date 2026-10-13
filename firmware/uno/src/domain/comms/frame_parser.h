#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Counters. bad_crc / bad_len count rejection events: a replayed byte can be rejected again
// and is then counted again.
struct ParseErrors {
  uint16_t bad_crc;
  uint16_t bad_len;
  uint16_t unknown_type;
  uint16_t tx_dropped;
};

// Byte-at-a-time parser for 0xAA | len | type | seq | payload | crc8, resyncing like the Plan A
// reference (conqueror_protocol.framing.FrameParser):
//  - a length byte > kMaxPayload that is itself 0xAA is taken as a new sync (stay in kLen);
//  - after a CRC mismatch the bytes received after the false sync are rescanned for the next
//    0xAA and replayed, so a good frame hidden behind a bad one is not lost.
// One buffer holds the raw bytes after sync (len, type, seq, payload, crc; at most 52); bytes
// waiting to be replayed live in the same buffer, behind the write position. The frame accessors
// read that buffer (dispatch by reference, no copy) and are valid only until the next
// feed()/next() call.
//
// Work is resumable and bounded by the caller. One received byte can cost up to ~700 parser
// steps and ~26 CRC passes (~676 CRC bytes) of replay, so next() runs at most `budget` steps
// (decremented in place) and keeps the rest pending for the next call:
//   if (!parser.pending()) parser.feed(b);           // feed() only when nothing is pending
//   if (parser.next(budget)) dispatch(parser);       // repeat while budget && pending()
// feed() never discards pending replay bytes: while pending() it refuses the byte (returns
// false) and changes nothing. Same frames, order and counters as the Python parser for any
// stream and any budget split.
class FrameParser {
 public:
  static constexpr uint8_t kRawMax = proto::kMaxPayload + 4;  // len, type, seq, payload, crc

  bool feed(uint8_t b);           // queue one received byte; false (byte refused) if pending()
  bool next(uint8_t& budget);     // run <= budget steps (decremented); true when a frame is ready
  bool pending() const { return m_rd < m_end; }  // replay or fed bytes still to be stepped
  uint8_t len() const { return m_raw[0]; }
  uint8_t type() const { return m_raw[1]; }
  uint8_t seq() const { return m_raw[2]; }
  const uint8_t* payload() const { return m_raw + 3; }
  ParseErrors& errors() { return m_errors; }
#ifdef CQ_HOST_TEST
  uint32_t stepsTaken() const { return m_steps; }  // host-test instrumentation only
#endif

 private:
  enum class State : uint8_t { kSync, kLen, kType, kSeq, kPayload, kCrc };
  bool step(uint8_t b);
  void resync();

  State m_state = State::kSync;
  uint8_t m_n = 0;    // raw bytes stored since the (possibly false) sync
  uint8_t m_rd = 0;   // next byte to step
  uint8_t m_end = 0;  // end of the bytes waiting to be stepped; while pending, m_n <= m_rd
  uint8_t m_raw[kRawMax] = {};
  ParseErrors m_errors{};
#ifdef CQ_HOST_TEST
  uint32_t m_steps = 0;
#endif
};

}  // namespace cq
