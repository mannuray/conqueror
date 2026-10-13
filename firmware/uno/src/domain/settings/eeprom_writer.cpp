#include "domain/settings/eeprom_writer.h"

namespace cq {

bool EepromWriter::enqueue(uint16_t addr, const uint8_t* data, uint8_t len) {
  if (m_range_count == kMaxRanges || static_cast<uint16_t>(m_used) + len > kMaxBytes) return false;
  for (uint8_t i = 0; i < len; ++i) m_data[m_used + i] = data[i];
  m_ranges[m_range_count++] = Range{addr, m_used, len};
  m_used = static_cast<uint8_t>(m_used + len);
  return true;
}

bool EepromWriter::step(const EepromIo& io) {
  while (m_range_count > 0) {
    Range& r = m_ranges[0];
    if (m_cursor == r.len) {  // range finished: drop it
      for (uint8_t i = 1; i < m_range_count; ++i) m_ranges[i - 1] = m_ranges[i];
      --m_range_count;
      m_cursor = 0;
      if (m_range_count == 0) m_used = 0;  // ranges are only appended between saves
      continue;
    }
    if (io.busy()) return false;
    const uint16_t addr = static_cast<uint16_t>(r.addr + m_cursor);
    const uint8_t value = m_data[r.offset + m_cursor];
    if (io.read(addr) == value) {  // unchanged byte: skip, try the next one this pass
      ++m_cursor;
      continue;
    }
    if (!io.writeAsync(addr, value)) return false;
    ++m_cursor;
    return false;  // one write per step; the next step drops a finished range
  }
  return true;
}

}  // namespace cq
