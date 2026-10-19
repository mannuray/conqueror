// Tank Logo notes (words.yaml presets.notes, Plan N2): C4..B6 with sharps (C#4) and R (rest); a number after a
// note is its length in quarter beats (default 1 = 250 ms, 0.25 to 16). The checker reads a PLAY list with it
// before anything moves; the runner turns it into TONE notes. Pure C++, fixed capacity.
#pragma once

#include <stdint.h>

namespace cq {
namespace logo {

struct Note {
  uint16_t hz = 0;  // 0 = rest
  uint16_t ms = 250;
};

// The frequency of one note name (case-insensitive); false when the text is not a note. R gives 0.
bool noteHz(const char* text, uint16_t len, uint16_t& hz);

class NoteReader {
 public:
  static constexpr uint8_t kMaxNotes = 64;
  static constexpr int32_t kBeatMinMilli = 250;
  static constexpr int32_t kBeatMaxMilli = 16000;
  enum class Err : uint8_t { kNone, kNotANote, kLength, kTooMany };

  Err word(const char* text, uint16_t len);
  Err number(int32_t beats_milli);  // the length of the note before it
  uint8_t count() const { return m_count; }
  const Note* notes() const { return m_notes; }

 private:
  Note m_notes[kMaxNotes];
  uint8_t m_count = 0;
  bool m_has_len = false;
};

}  // namespace logo
}  // namespace cq
