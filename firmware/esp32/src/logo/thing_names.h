// Loose names for things (Tank Logo LLD §24.1): the AI helper says which words it can find and what each is also
// called ("toy" = car, racing car). The checker resolves a written name to its word. Fixed arrays, no heap.
#pragma once

#include <stdint.h>

namespace cq {
namespace logo {

class ThingNames {
 public:
  static constexpr uint8_t kMaxWords = 48, kMaxNames = 8, kMaxLen = 24;

  // words[i] has n_names[i] names, found at names[i * kMaxNames + j]. The word itself resolves too.
  // Returns false and keeps the old list when over a bound (words, names per word, bytes per word or name).
  bool set(const char* const* words, const uint8_t* n_names, const char* const* names, uint8_t n_words);
  // Lower-cases and squeezes spaces; the word a name stands for, or nullptr.
  const char* resolve(const char* name) const;
  uint8_t size() const { return m_n_words; }
  void clear() { m_n_words = 0; m_n_entries = 0; }

 private:
  struct Entry {
    uint8_t word;
    char text[kMaxLen + 1];
  };
  static bool textOk(const char* s);
  static bool wordOk(const char* word, uint8_t n_names, const char* const* names);
  static bool normalize(const char* in, char* out, uint16_t in_len);  // false when over kMaxLen bytes

  char m_words[kMaxWords][kMaxLen + 1] = {};
  Entry m_entries[kMaxWords * kMaxNames] = {};
  uint8_t m_n_words = 0;
  uint16_t m_n_entries = 0;
};

}  // namespace logo
}  // namespace cq
