#include "logo/words.h"

#include <string.h>

namespace cq {
namespace logo {
namespace {

struct List {
  const char* const* items;
  uint8_t n;
};

List presets(gen::InType place) {
  switch (place) {  // VARIANT POINT: preset places
    case gen::InType::kSide: return {gen::kSides, gen::kSidesCount};
    case gen::InType::kColour: return {gen::kColours, gen::kColoursCount};
    case gen::InType::kLook: return {gen::kLooks, gen::kLooksCount};
    case gen::InType::kSound: return {gen::kSounds, gen::kSoundsCount};
    default: return {nullptr, 0};
  }
}

bool inList(const char* src, const Token& t, List l) {
  for (uint8_t i = 0; i < l.n; ++i) {
    if (tokenIs(src, t, l.items[i])) return true;
  }
  return false;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

}  // namespace

int16_t findWord(const char* src, const Token& t) {
  if (t.kind != Tok::kName) return -1;
  for (uint8_t i = 0; i < gen::kWordCount; ++i) {
    if (tokenIs(src, t, gen::kWords[i].name)) return i;
  }
  for (uint8_t i = 0; i < gen::kAliasCount; ++i) {
    if (tokenIs(src, t, gen::kAliases[i].alias)) return gen::kAliases[i].word;
  }
  return -1;
}

int16_t wordIndex(const char* name) {
  for (uint8_t i = 0; i < gen::kWordCount; ++i) {
    if (strcmp(gen::kWords[i].name, name) == 0) return i;
  }
  return -1;
}

bool isPresetOf(const char* src, const Token& t, gen::InType place) { return inList(src, t, presets(place)); }

bool isAnyPreset(const char* src, const Token& t) {
  return isPresetOf(src, t, gen::InType::kSide) || isPresetOf(src, t, gen::InType::kColour) ||
         isPresetOf(src, t, gen::InType::kLook) || isPresetOf(src, t, gen::InType::kSound);
}

bool isReserved(const char* src, const Token& t) { return inList(src, t, {gen::kReserved, gen::kReservedCount}); }

uint8_t editDistance(const char* a, uint16_t alen, const char* b, uint16_t blen) {
  if (alen > 32 || blen > 32) return 3;
  uint8_t prev[33], cur[33];
  for (uint16_t j = 0; j <= blen; ++j) prev[j] = static_cast<uint8_t>(j);
  for (uint16_t i = 1; i <= alen; ++i) {
    cur[0] = static_cast<uint8_t>(i);
    for (uint16_t j = 1; j <= blen; ++j) {
      const uint8_t cost = upper(a[i - 1]) == upper(b[j - 1]) ? 0 : 1;
      uint8_t v = static_cast<uint8_t>(prev[j] + 1);
      if (cur[j - 1] + 1 < v) v = static_cast<uint8_t>(cur[j - 1] + 1);
      if (prev[j - 1] + cost < v) v = static_cast<uint8_t>(prev[j - 1] + cost);
      cur[j] = v;
    }
    memcpy(prev, cur, blen + 1u);
  }
  return prev[blen] > 3 ? 3 : prev[blen];
}

void presetChoices(gen::InType place, char* out, size_t cap) {
  const List l = presets(place);
  out[0] = '\0';
  for (uint8_t i = 0; i < l.n && i < 8; ++i) {
    if (i > 0) strncat(out, " ", cap - strlen(out) - 1);
    strncat(out, l.items[i], cap - strlen(out) - 1);
  }
  if (l.n > 8) strncat(out, " …", cap - strlen(out) - 1);
}

}  // namespace logo
}  // namespace cq
