#include "domain/ui/sound_sequencer.h"

#include "domain/util/math_util.h"

namespace cq {

bool SoundSequencer::play(const Note* notes, uint8_t n, uint8_t priority, uint32_t now_ms) {
  if (n == 0 || m_mode == proto::AudioModeValue::kMute) return false;
  if (m_mode == proto::AudioModeValue::kQuiet && priority < kPrioAlarm) return false;
  if (playing() && priority < m_prio) return false;
  if (n > kMaxNotes) n = kMaxNotes;
  for (uint8_t i = 0; i < n; ++i) m_notes[i] = notes[i];
  m_count = n;
  m_idx = 0;
  m_prio = priority;
  m_note_start_ms = now_ms;
  m_started = false;
  return true;
}

void SoundSequencer::setMode(proto::AudioModeValue m) {
  m_mode = m;
  if (m == proto::AudioModeValue::kMute) stop();
}

void SoundSequencer::stop() {
  m_count = 0;
  m_idx = 0;
  m_prio = 0;
}

bool SoundSequencer::tick(uint32_t now_ms, uint16_t& hz) {
  uint16_t want = 0;
  if (m_count > 0) {
    if (!m_started) {
      m_started = true;
      m_note_start_ms = now_ms;
    }
    while (m_count > 0 && elapsed(now_ms, m_note_start_ms, m_notes[m_idx].ms)) {
      m_note_start_ms += m_notes[m_idx].ms;
      if (++m_idx >= m_count) stop();
    }
    if (m_count > 0) want = m_notes[m_idx].hz;
  }
  hz = want;
  if (want == m_out_hz) return false;
  m_out_hz = want;
  return true;
}

}  // namespace cq
