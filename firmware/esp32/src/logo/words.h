// Lookups over the generated word table (LLD §4): names, aliases, presets, reserved words, edit distance.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/generated/words.h"
#include "logo/lexer.h"

namespace cq {
namespace logo {

int16_t findWord(const char* src, const Token& t);  // name or alias -> gen::kWords index, -1
int16_t wordIndex(const char* name);                // exact canonical name -> index, -1
bool isPresetOf(const char* src, const Token& t, gen::InType place);
bool isAnyPreset(const char* src, const Token& t);
bool isReserved(const char* src, const Token& t);
uint8_t editDistance(const char* a, uint16_t alen, const char* b, uint16_t blen);  // case-insensitive, max 3
void presetChoices(gen::InType place, char* out, size_t cap);                      // "RED GREEN …"
bool isPersonWord(const char* word, size_t len);  // one of words.yaml's person_words, any case (Tank Logo LLD §27.1)
bool isPersonWord(const char* word);              // a NUL-terminated word

}  // namespace logo
}  // namespace cq
