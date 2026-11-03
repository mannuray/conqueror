// The AI helper's things as parsed from a hello / skill_things frame (bridge LLD R26.2): views into the frame's text,
// laid out the way logo::ThingNames::set wants them (word w's j-th name is names[w * kNameStride + j]).
// ~1.7 KB on the ESP32: keep it in a long-lived object, not on a small task stack.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

struct ThingList {
  static constexpr uint8_t kMaxWords = 48, kNameStride = 8;
  uint8_t n = 0;
  const char* word[kMaxWords] = {};
  uint8_t n_names[kMaxWords] = {};
  const char* names[kMaxWords * kNameStride] = {};
  void clear() {  // in place: no temporary of this size on a stack
    n = 0;
    for (uint8_t i = 0; i < kMaxWords; ++i) {
      word[i] = nullptr;
      n_names[i] = 0;
    }
    for (uint16_t i = 0; i < kMaxWords * kNameStride; ++i) names[i] = nullptr;
  }
};

enum class ThingParse : uint8_t { kOk, kBad, kTooBig };

// Finds "things" in the JSON object `text` and fills `out`. The text is changed in place (each string's closing quote
// becomes a NUL, so the views are C strings): pass the frame buffer, and keep it alive as long as `out` is used.
// No "things" key: an empty list, kOk. kBad: not an array of {word, names?} with plain strings (no escapes).
// kTooBig: more than 48 words or 8 names for a word (the byte lengths are ThingNames' to check).
ThingParse parseThingList(char* text, size_t len, ThingList& out);

}  // namespace core
}  // namespace cq
