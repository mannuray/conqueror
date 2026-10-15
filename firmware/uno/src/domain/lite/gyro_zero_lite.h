#pragma once
#include <stdint.h>

namespace cq {

// Learns the Z gyro zero while the tank stands still (LLD §11.8 rules, Lite: Z only, integers).
// Quiet 1 s, then 2 s windows (400 samples at 200 Hz). Valid window: spread ≤ 98 raw (1.5 °/s), |mean| ≤ 1310 raw
// (20 °/s). Two valid windows in a row within 20 raw (0.3 °/s) → zero = their average. RAM only.
class GyroZeroLite {
 public:
  static constexpr uint16_t kSettleMs = 1000;
  static constexpr uint16_t kWindowSamples = 400;
  static constexpr int16_t kMaxSpreadRaw = 98;
  static constexpr int16_t kMaxZeroRaw = 1310;
  static constexpr int16_t kAgreeRaw = 20;
  void seed(int16_t stored_raw);  // restarts only when the stored value changed (applySettings runs often)
  void update(int16_t raw, bool quiet, uint32_t now_ms);
  int16_t zero() const { return m_zero; }
  uint16_t accepted() const { return m_accepted; }

 private:
  enum class State : uint8_t { kWaitQuiet, kSettle, kCollect };
  void startWindow();
  void judge();
  State m_state = State::kWaitQuiet;
  uint32_t m_quiet_since_ms = 0;
  uint16_t m_n = 0;
  int32_t m_sum = 0;
  int16_t m_min = 0;
  int16_t m_max = 0;
  bool m_have_prev = false;
  int16_t m_prev = 0;
  int16_t m_zero = 0;
  int16_t m_seeded = 0;
  bool m_seeded_valid = false;
  uint16_t m_accepted = 0;
};

}  // namespace cq
