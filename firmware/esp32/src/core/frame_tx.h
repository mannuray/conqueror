// Whole-chunk TX queue in front of the Uno UART (bridge LLD R2.8). Every writer hands over complete
// chunks (one frame, or one run of raw passthrough bytes); the single drainer (link_task) writes a
// chunk only when the UART can take all of it, so chunks never interleave mid-frame. One urgent
// slot (STOP) goes out at the next chunk boundary, ahead of queued chunks (fast path, R2.8): it
// never cuts the chunk being written, and it purges queued motion chunks (DRIVE / CAM_JOY) that
// have not started, so an older drive can never follow the STOP and restart the tank. Passthrough
// (bench) chunks are pushed as kKeep and never purged. Pure C++, no heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

enum class ChunkClass : uint8_t { kKeep, kMotion };  // kMotion: purged by a STOP (pushUrgent)

class FrameTx {
 public:
  static constexpr size_t kMaxChunk = 64;  // > largest frame (53 B)
  static constexpr size_t kSlots = 32;

  // false (and counted) when full or too long
  bool push(const uint8_t* data, size_t len, ChunkClass cls = ChunkClass::kKeep);
  // STOP fast path: purges queued kMotion chunks not yet started, then fills the urgent slot
  // (replaces a not-yet-started urgent chunk; false while an urgent chunk is being written).
  bool pushUrgent(const uint8_t* data, size_t len);
  size_t peek(const uint8_t*& data);  // next chunk (0 = empty); stays until pop()
  void pop();                         // the peeked chunk was written whole
  void clear();  // drop queued chunks not yet peeked; the urgent slot (STOP) is kept
  size_t queued() const { return m_count + (m_urgent_len ? 1 : 0); }
  uint32_t dropped() const { return m_dropped; }
  uint32_t purged() const { return m_purged; }  // motion chunks removed by a STOP

 private:
  enum class Inflight : uint8_t { kNone, kQueue, kUrgent };
  void purgeMotion();
  uint8_t m_data[kSlots][kMaxChunk] = {};
  uint8_t m_len[kSlots] = {};
  ChunkClass m_cls[kSlots] = {};
  size_t m_head = 0;
  size_t m_count = 0;
  uint8_t m_urgent[kMaxChunk] = {};
  size_t m_urgent_len = 0;
  Inflight m_inflight = Inflight::kNone;
  uint32_t m_dropped = 0;
  uint32_t m_purged = 0;
};

}  // namespace core
}  // namespace cq
