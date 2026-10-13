// Boot counter + the last kSize reset reasons, newest first (design-notes §38: find out why the
// ESP32 does not start on tank power). Pure C++; the shell stores count/ring/head in NVS.
// Reason codes are esp_reset_reason_t values (0 unknown, 1 power-on, ... 9 brownout ...).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

class BootHistory {
 public:
  static constexpr size_t kSize = 8;

  void restore(uint32_t count, const uint8_t* ring, uint8_t head) {
    m_count = count;
    for (size_t i = 0; i < kSize; ++i) m_ring[i] = ring[i];
    m_head = static_cast<uint8_t>(head % kSize);
  }
  void push(uint8_t reason) {
    m_ring[m_head] = reason;
    m_head = static_cast<uint8_t>((m_head + 1) % kSize);
    ++m_count;
  }
  // Newest first; returns how many were written (min(count, kSize, max)).
  size_t recent(uint8_t* out, size_t max) const {
    const size_t have = m_count < kSize ? static_cast<size_t>(m_count) : kSize;
    const size_t n = have < max ? have : max;
    for (size_t i = 0; i < n; ++i) out[i] = m_ring[(m_head + kSize - 1 - i) % kSize];
    return n;
  }
  uint32_t count() const { return m_count; }
  const uint8_t* ring() const { return m_ring; }
  uint8_t head() const { return m_head; }

  static const char* reasonName(uint8_t r) {
    static const char* const kNames[] = {"unknown",  "power_on",  "external", "software",
                                         "panic",    "int_wdt",   "task_wdt", "wdt",
                                         "deepsleep", "brownout", "sdio",     "usb",
                                         "jtag",     "efuse",     "power_glitch", "cpu_lockup"};
    return r < sizeof(kNames) / sizeof(kNames[0]) ? kNames[r] : "unknown";
  }

 private:
  uint32_t m_count = 0;
  uint8_t m_ring[kSize] = {};
  uint8_t m_head = 0;
};

}  // namespace core
}  // namespace cq
