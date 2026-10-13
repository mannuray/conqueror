#pragma once
#include <stdint.h>

namespace cq {

// Boot self-test (LLD §7, §10.9). Critical = IMU missing/failed (no tilt/lift safety, no heading hold) and an
// implausible battery reading (divider/wiring fault: battery safety blind). A critical result latches the
// motion lock (SafetyInputs.selftest_locked) until a later run passes or the Uno reboots; Override never
// lifts it. A lock caused ONLY by the battery check clears by itself after 1 s of valid readings (tickBattery,
// owner 2026-10-12); an IMU failure never auto-clears. Ultrasonic and settings (defaults are compiled in, always valid) are warnings only.
class SelfTest {
 public:
  enum class Check : uint8_t { kImu = 0, kUltrasonic = 1, kBattery = 2, kSettings = 3 };
  static constexpr uint8_t kCriticalMask = (1u << 0) | (1u << 2);  // kImu | kBattery
  static constexpr uint16_t kBattMinMv = 5000;  // 2S pack reads 6.0..8.4 V; outside 5..9 V = implausible
  static constexpr uint16_t kBattMaxMv = 9000;
  static constexpr uint32_t kBattRecoverMs = 1000;  // battery valid this long (continuously) → battery-only lock clears
  static bool batteryPlausible(uint16_t mv) { return mv >= kBattMinMv && mv <= kBattMaxMv; }

  void begin() {  // a run starts (boot or SELF_TEST re-run); the lock stays as it is until complete()
    m_fail = 0;
    m_running = true;
  }
  void report(Check c, bool ok) {
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(c));
    m_fail = ok ? static_cast<uint8_t>(m_fail & ~bit) : static_cast<uint8_t>(m_fail | bit);
  }
  void complete() {  // the run's verdict: lock on a critical failure, unlock on a pass
    m_running = false;
    m_locked = critical();
    if (m_runs < 255) ++m_runs;
  }
  // Battery-only auto-recovery: call ~10 Hz with the current battery verdict. Acts only while locked, no run in
  // progress, and the critical failure is exactly the battery bit (an IMU failure needs an explicit re-run).
  // Returns true on the tick that clears the lock (the app then emits EVENT SELFTEST / clears the flag).
  bool tickBattery(bool plausible, uint32_t now_ms) {
    constexpr uint8_t kBattBit = 1u << static_cast<uint8_t>(Check::kBattery);
    if (!m_locked || m_running || (m_fail & kCriticalMask) != kBattBit || !plausible) {
      m_batt_valid = false;  // any break (or a non-eligible state) restarts the 1 s
      return false;
    }
    if (!m_batt_valid) {
      m_batt_valid = true;
      m_batt_since = now_ms;
      return false;
    }
    if (static_cast<uint32_t>(now_ms - m_batt_since) < kBattRecoverMs) return false;
    m_fail = static_cast<uint8_t>(m_fail & ~kBattBit);
    m_locked = critical();  // false: the battery bit was the only critical one
    m_batt_valid = false;
    if (m_runs < 255) ++m_runs;  // counts like a passing run: the UI replays SUCCESS
    return true;
  }
  uint8_t failMask() const { return m_fail; }
  bool critical() const { return (m_fail & kCriticalMask) != 0; }
  bool locked() const { return m_locked; }
  bool running() const { return m_running; }
  uint8_t runs() const { return m_runs; }  // completed runs (the UI replays the result sound on change)

 private:
  uint8_t m_fail = 0;
  uint8_t m_runs = 0;
  bool m_locked = false;
  bool m_running = false;
  bool m_batt_valid = false;
  uint32_t m_batt_since = 0;
};

}  // namespace cq
