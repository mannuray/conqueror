#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct Note {
  uint16_t hz;  // 0 = rest
  uint16_t ms;
};

// Non-blocking note player for the passive buzzer. Higher priority interrupts; QUIET plays alarms only.
class SoundSequencer {
 public:
  static constexpr uint8_t kMaxNotes = 14;  // STARTUP (old boot sweep) has 14; TONE stays <= 10
  static constexpr uint8_t kPrioInfo = 1;
  static constexpr uint8_t kPrioEvent = 2;
  static constexpr uint8_t kPrioAlarm = 3;

  bool play(const Note* notes, uint8_t n, uint8_t priority, uint32_t now_ms);
  void setMode(proto::AudioModeValue m);
  proto::AudioModeValue mode() const { return m_mode; }
  void stop();
  bool tick(uint32_t now_ms, uint16_t& hz);
  bool playing() const { return m_count > 0; }

 private:
  Note m_notes[kMaxNotes] = {};
  uint8_t m_count = 0;
  uint8_t m_idx = 0;
  uint8_t m_prio = 0;
  uint32_t m_note_start_ms = 0;
  bool m_started = false;
  uint16_t m_out_hz = 0;
  proto::AudioModeValue m_mode = proto::AudioModeValue::kNormal;
};

}  // namespace cq
