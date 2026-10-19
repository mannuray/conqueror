#include "logo/program_names.h"

#include <string.h>

namespace cq {
namespace logo {
namespace {

constexpr Example kExamples[] = {
    {"first_square", "; Your first square\nREPEAT 4 [ FORWARD 30 RIGHT 90 ]\nBEEP\n"},
    {"avoid_walls",
     "; Drive around and turn away from walls\nFOREVER [\n  FORWARD 20\n  IF BLOCKED? [ BACK 10 RIGHT 90 ]\n]\n"},
    {"look_around", "; Look around, then say so\nLOOK LEFT WAIT 1\nLOOK RIGHT WAIT 1\nLOOK HOME\nSAY [ all clear ]\n"},
};

bool nameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

}  // namespace

bool programName(const char* in, char* out, size_t cap) {
  const size_t n = strlen(in);
  if (n == 0 || n > kNameMax || cap <= n) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = (in[i] >= 'A' && in[i] <= 'Z') ? static_cast<char>(in[i] + 32) : in[i];
    if (!nameChar(c)) return false;
    out[i] = c;
  }
  out[n] = '\0';
  return true;
}

uint8_t exampleCount() { return static_cast<uint8_t>(sizeof kExamples / sizeof kExamples[0]); }
const Example& example(uint8_t i) { return kExamples[i]; }

const Example* findExample(const char* lower_name) {
  for (const Example& e : kExamples) {
    if (strcmp(e.name, lower_name) == 0) return &e;
  }
  return nullptr;
}

}  // namespace logo
}  // namespace cq
