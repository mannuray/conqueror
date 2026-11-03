#include "logo/thing_names.h"

#include <string.h>

namespace cq {
namespace logo {

namespace {
constexpr uint16_t kScan = 256;  // longest text looked at; anything longer is over kMaxLen anyway
}

// Lower case, spaces squeezed to one, none at the ends. in_len bytes at most (0 = up to the end).
bool ThingNames::normalize(const char* in, char* out, uint16_t in_len) {
  uint8_t n = 0;
  bool pending_space = false;
  for (uint16_t i = 0; i < in_len && in[i] != '\0'; ++i) {
    char c = in[i];
    if (c == ' ' || c == '\t') {
      pending_space = n > 0;
      continue;
    }
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (pending_space) {
      if (n >= kMaxLen) return false;
      out[n++] = ' ';
      pending_space = false;
    }
    if (n >= kMaxLen) return false;
    out[n++] = c;
  }
  out[n] = '\0';
  return true;
}

bool ThingNames::isPerson(const char* s) {
  char tmp[kMaxLen + 1];
  return normalize(s, tmp, kScan) && strcmp(tmp, "person") == 0;
}

// One text (a word or a name) that fits and is not empty.
bool ThingNames::textOk(const char* s) {
  char tmp[kMaxLen + 1];
  return s != nullptr && normalize(s, tmp, kScan) && tmp[0] != '\0';
}

bool ThingNames::wordOk(const char* word, uint8_t n_names, const char* const* names) {
  if (n_names > kMaxNames || !textOk(word)) return false;
  for (uint8_t j = 0; j < n_names; ++j) {
    if (!textOk(names[j])) return false;
  }
  return true;
}

// Stores one checked word with its names; "person" is skipped.
void ThingNames::add(const char* word, uint8_t n_names, const char* const* names) {
  if (isPerson(word)) return;
  const uint8_t id = m_n_words++;
  normalize(word, m_words[id], kScan);
  for (uint8_t j = 0; j < n_names; ++j) {
    if (isPerson(names[j])) continue;
    Entry& e = m_entries[m_n_entries++];
    e.word = id;
    normalize(names[j], e.text, kScan);
  }
}

bool ThingNames::set(const char* const* words, const uint8_t* n_names, const char* const* names, uint8_t n_words) {
  if (n_words > kMaxWords || (n_words > 0 && (words == nullptr || n_names == nullptr))) return false;
  for (uint8_t w = 0; w < n_words; ++w) {  // check everything before changing anything
    if (!wordOk(words[w], n_names[w], names + w * kMaxNames)) return false;
  }
  clear();
  for (uint8_t w = 0; w < n_words; ++w) add(words[w], n_names[w], names + w * kMaxNames);
  return true;
}

const char* ThingNames::resolve(const char* name) const {
  if (name == nullptr) return nullptr;
  char key[kMaxLen + 1];
  if (!normalize(name, key, kScan) || key[0] == '\0') return nullptr;
  for (uint8_t w = 0; w < m_n_words; ++w) {
    if (strcmp(key, m_words[w]) == 0) return m_words[w];
  }
  for (uint16_t i = 0; i < m_n_entries; ++i) {
    if (strcmp(key, m_entries[i].text) == 0) return m_words[m_entries[i].word];
  }
  return nullptr;
}

}  // namespace logo
}  // namespace cq
