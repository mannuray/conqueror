// Saved program names and the built-in examples (Tank Logo LLD §12, §20.4): 1-24 characters a-z 0-9 _ -, without
// regard to case (kept in lower case); examples and lessons (§25) are read-only. Pure.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace logo {

constexpr size_t kNameMax = 24;
constexpr uint8_t kMaxPrograms = 32;

// The name in lower case into out (cap ≥ 25); false when it is not a valid name.
bool programName(const char* in, char* out, size_t cap);

// The built-ins: the five lessons in order (Tank Logo LLD §25), then the examples.
constexpr uint8_t kBuiltInCount = 9;

struct Example {
  const char* name;
  const char* src;
  bool lesson;
};
uint8_t exampleCount();
const Example& example(uint8_t i);
const Example* findExample(const char* lower_name);

}  // namespace logo
}  // namespace cq
