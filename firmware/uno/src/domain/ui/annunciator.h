#pragma once
#include <stdint.h>

#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"

namespace cq {

struct AnnState {
  bool synced;
  bool calibrating;
  bool blocked;
  bool lifted;
  bool tilted;
  bool batt_low;
  bool batt_crit;
  bool selftest_critical;
  bool reversing;
  bool forward;
  bool parking_beeps;
  proto::Owner owner;
  proto::OwnerHintValue hint;
  uint8_t bump_seq;
  uint8_t selftest_runs;  // SelfTest::runs(): +1 per completed run (boot or SELF_TEST re-run)
  bool selftest_failed;
  // LLD §10.6 (Revision 3)
  bool override_on;      // SafetyOverride::active()
  uint8_t override_seq;  // +1 per Override enter/exit → chirp + toggle blinks
  uint8_t toggle_seq;    // +1 per IR toggle (mute 7, parking-beeps * tap) → toggle blinks
  bool toggle_on;        // state after that toggle (green = on, red = off)
  bool driving;          // tracks moving (base colour scales with speed instead of breathing)
  uint8_t speed_pct;     // max(|L|, |R|) output, %
  bool usb_power;        // USB only, no battery (spec 02 §D6.3): white flash every 2 s over the base
};

// Turns robot state into LED layers and sounds (edges only; no event hooks needed).
class Annunciator {
 public:
  static constexpr Rgb kColorNoLink{0, 0, 120};    // blue, breathing when idle: no ESP32 link yet (spec 02)
  // Owner colours per LLD §9.9: IR purple, network human green, network AI cyan.
  static constexpr Rgb kColorReady{20, 20, 20};    // dim white: idle, ESP32 synced
  static constexpr Rgb kColorHuman{0, 120, 0};     // green
  static constexpr Rgb kColorAi{0, 100, 100};      // cyan
  static constexpr Rgb kColorIr{80, 0, 120};       // purple
  static constexpr Rgb kColorCal{120, 100, 0};     // yellow blink
  static constexpr Rgb kColorAlarm{255, 0, 0};
  static constexpr Rgb kColorUsb{60, 60, 60};     // white: 150 ms flash every 2 s on USB power
  static constexpr Rgb kColorLow{200, 110, 0};     // amber: short flash every 2 s over the base (spec 02)
  static constexpr uint16_t kLowFlashMs = 150;
  static constexpr Rgb kColorOverride{255, 60, 0}; // amber-red pulse while Safety Override is on
  static constexpr Rgb kColorOn{0, 255, 0};        // toggle confirm: on
  static constexpr uint16_t kParkingBeepMs = 600;
  static constexpr uint16_t kTiltSirenMs = 400;     // safety chirp repeat while tilted
  static constexpr uint16_t kToggleBlinkMs = 480;   // 3 × (80 on + 80 off)

  void update(const AnnState& s, uint32_t now_ms, LedSequencer& led, SoundSequencer& snd);

 private:
  void sound(proto::SoundId id, uint8_t prio, uint32_t now_ms, SoundSequencer& snd);
  AnnState m_prev{};
  bool m_primed = false;
  uint32_t m_last_beep_ms = 0;
  uint32_t m_last_crit_ms = 0;
  uint32_t m_last_siren_ms = 0;
  uint32_t m_toggle_since_ms = 0;
  bool m_toggle_active = false;
  bool m_prev_base_set = false;
  void pickAttention(const AnnState& s, uint32_t now_ms, LedSequencer& led);
};

}  // namespace cq
