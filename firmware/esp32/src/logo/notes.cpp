#include "logo/notes.h"

namespace cq {
namespace logo {
namespace {

// Octave 4 in 0.01 Hz, C to B (equal temperament, A4 = 440 Hz).
constexpr uint32_t kOct4CentiHz[] = {26163, 27718, 29366, 31113, 32963, 34923, 36999, 39200, 41530, 44000, 46616, 49388};
// Semitone of each letter A..G.
constexpr int8_t kLetter[] = {9, 11, 0, 2, 4, 5, 7};

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

// The semitone (0 = C .. 11 = B) and octave of "C4" / "C#4"; false when it is not one of C4..B6.
bool parseNote(const char* text, uint16_t len, int& semi, int& oct) {
  const char letter = upper(text[0]);
  if (len < 2 || len > 3 || letter < 'A' || letter > 'G') return false;
  const bool sharp = len == 3;
  if (sharp && text[1] != '#') return false;
  oct = text[len - 1] - '0';
  semi = kLetter[letter - 'A'] + (sharp ? 1 : 0);
  return oct >= 4 && oct <= 6 && semi <= 11;  // no B#
}

}  // namespace

bool noteHz(const char* text, uint16_t len, uint16_t& hz) {
  if (len == 1 && upper(text[0]) == 'R') {
    hz = 0;
    return true;
  }
  int semi = 0, oct = 0;
  if (!parseNote(text, len, semi, oct)) return false;
  hz = static_cast<uint16_t>((kOct4CentiHz[semi] * (1u << (oct - 4)) + 50) / 100);
  return true;
}

NoteReader::Err NoteReader::word(const char* text, uint16_t len) {
  uint16_t hz = 0;
  if (!noteHz(text, len, hz)) return Err::kNotANote;
  if (m_count == kMaxNotes) return Err::kTooMany;
  m_notes[m_count++] = Note{hz, 250};
  m_has_len = false;
  return Err::kNone;
}

NoteReader::Err NoteReader::number(int32_t beats_milli) {
  if (m_count == 0 || m_has_len) return Err::kNotANote;
  if (beats_milli < kBeatMinMilli || beats_milli > kBeatMaxMilli) return Err::kLength;
  m_notes[m_count - 1].ms = static_cast<uint16_t>(beats_milli / 4);  // 1000 milli-beats = 250 ms
  m_has_len = true;
  return Err::kNone;
}

}  // namespace logo
}  // namespace cq
