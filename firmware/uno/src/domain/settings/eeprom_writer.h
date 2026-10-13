#pragma once
#include <stdint.h>

namespace cq {

struct EepromIo {
  uint8_t (*read)(uint16_t addr);
  bool (*writeAsync)(uint16_t addr, uint8_t value);  // false if the previous write is still running
  bool (*busy)();
};

// Writes queued byte ranges at most one byte per step(); unchanged bytes are skipped.
// RAM: 80 B data + 4 ranges × 4 B (one EEPROM address per range, not per byte). SettingsSaver queues one
// block at a time (≤ magic 3 + global 66 = 69 B), checked by static_asserts (LLD §10.10).
class EepromWriter {
 public:
  static constexpr uint8_t kMaxBytes = 80;
  static constexpr uint8_t kMaxRanges = 4;
  bool enqueue(uint16_t addr, const uint8_t* data, uint8_t len);  // false if full
  bool step(const EepromIo& io);                                   // true when idle
  bool idle() const { return m_range_count == 0; }

 private:
  struct Range {
    uint16_t addr;
    uint8_t offset;  // into m_data
    uint8_t len;
  };
  uint8_t m_data[kMaxBytes] = {};
  Range m_ranges[kMaxRanges] = {};
  uint8_t m_range_count = 0;
  uint8_t m_used = 0;    // bytes of m_data in use
  uint8_t m_cursor = 0;  // next byte inside m_ranges[0]
};

}  // namespace cq
