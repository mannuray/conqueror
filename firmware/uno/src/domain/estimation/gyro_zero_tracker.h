#pragma once
#include <stdint.h>

namespace cq {

// Learns the gyro zero (raw units, all three axes) whenever the tank stands still (LLD §11.8).
// Quiet for 1 s, then 2 s windows; a window is valid when every axis stays within 1.5 deg/s and the
// mean is within the datasheet's 20 deg/s. Two valid windows in a row that agree within 0.3 deg/s set
// the zero to their average. RAM only: seed() starts from the stored calibration settings.
class GyroZeroTracker {
 public:
  static constexpr uint16_t kSettleMs = 1000;
  static constexpr uint16_t kWindowSamples = 400;  // 2 s at 200 Hz
  static constexpr int16_t kMaxSpreadRaw = 98;     // 1.5 deg/s at 65.5 raw per deg/s
  static constexpr int16_t kMaxZeroRaw = 1310;     // 20 deg/s
  static constexpr int16_t kAgreeRaw = 20;         // 0.3 deg/s
  // Starts again from the stored zero, but only if it differs from the last one seeded: applySettings()
  // runs after every setting change and must not throw away a learned zero.
  void seed(const int16_t stored_raw[3]);
  void update(const int16_t raw[3], bool quiet, uint32_t now_ms);
  float zero(uint8_t axis) const { return m_zero[axis]; }
  uint16_t accepted() const { return m_accepted; }

 private:
  enum class State : uint8_t { kWaitQuiet, kSettle, kCollect };
  void startWindow();
  void judge();
  State m_state = State::kWaitQuiet;
  uint32_t m_quiet_since_ms = 0;
  uint16_t m_n = 0;
  int32_t m_sum[3] = {0, 0, 0};
  int16_t m_min[3] = {0, 0, 0};
  int16_t m_max[3] = {0, 0, 0};
  bool m_have_prev = false;
  float m_prev[3] = {0.0f, 0.0f, 0.0f};
  float m_zero[3] = {0.0f, 0.0f, 0.0f};
  int16_t m_seeded[3] = {0, 0, 0};
  bool m_seeded_valid = false;
  uint16_t m_accepted = 0;
};

}  // namespace cq
