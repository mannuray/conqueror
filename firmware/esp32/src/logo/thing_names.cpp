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
void ThingNames::store(const char* word, uint8_t n_names, const char* const* names) {
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
  for (uint8_t w = 0; w < n_words; ++w) store(words[w], n_names[w], names + w * kMaxNames);
  return true;
}

int ThingNames::findWord(const char* normalized) const {
  for (uint8_t w = 0; w < m_n_words; ++w) {
    if (strcmp(normalized, m_words[w]) == 0) return w;
  }
  return -1;
}

uint8_t ThingNames::namesOf(uint8_t word) const {
  uint8_t n = 0;
  for (uint16_t i = 0; i < m_n_entries; ++i) n = static_cast<uint8_t>(n + (m_entries[i].word == word ? 1 : 0));
  return n;
}

// Room for the word (if new) and its names (person is skipped) within the bounds, over everything added so far.
bool ThingNames::fits(int at, uint8_t n_names, const char* const* names) const {
  if (at < 0 && m_n_words >= kMaxWords) return false;
  uint8_t kept = at < 0 ? 0 : namesOf(static_cast<uint8_t>(at));
  for (uint8_t j = 0; j < n_names; ++j) kept = static_cast<uint8_t>(kept + (isPerson(names[j]) ? 0 : 1));
  return kept <= kMaxNames;
}

bool ThingNames::add(const char* word, const char* const* names, uint8_t n_names) {
  if ((n_names > 0 && names == nullptr) || !wordOk(word, n_names, names)) return false;
  if (isPerson(word)) return true;
  char key[kMaxLen + 1];
  normalize(word, key, kScan);
  const int at = findWord(key);
  if (!fits(at, n_names, names)) return false;
  if (at < 0) {
    store(word, n_names, names);
    return true;
  }
  for (uint8_t j = 0; j < n_names; ++j) {
    if (isPerson(names[j])) continue;
    Entry& e = m_entries[m_n_entries++];
    e.word = static_cast<uint8_t>(at);
    normalize(names[j], e.text, kScan);
  }
  return true;
}

void ThingNames::copyFrom(const ThingNames& o) {
  m_n_words = o.m_n_words;
  m_n_entries = o.m_n_entries;
  for (uint8_t w = 0; w < o.m_n_words; ++w) memcpy(m_words[w], o.m_words[w], sizeof m_words[w]);
  for (uint16_t i = 0; i < o.m_n_entries; ++i) m_entries[i] = o.m_entries[i];
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
