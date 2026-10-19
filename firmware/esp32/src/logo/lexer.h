// Tank Logo tokens (grammar.md §2, LLD §4). The whole source is tokenised once; the list always ends with kEnd.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/diag.h"

namespace cq {
namespace logo {

enum class Tok : uint8_t {
  kEnd, kNumber, kName, kInput, kQuoted, kPlaceholder, kLBracket, kRBracket, kLParen, kRParen,
  kPlus, kMinus, kNeg, kStar, kSlash, kEq, kNe, kLt, kGt, kLe, kGe,
};

struct Token {
  Tok kind = Tok::kEnd;
  uint16_t start = 0;  // byte offset in the source
  uint16_t len = 0;
  uint16_t line = 1;
  uint16_t col = 1;
  int32_t milli = 0;   // kNumber
  uint8_t ph = 0;      // kPlaceholder: 0 = ?, 1 = ?1, 2 = ?2
};

struct TokenList {
  static constexpr uint16_t kCap = 4096;
  Token t[kCap];
  uint16_t n = 0;
};

void lex(const char* src, uint16_t len, TokenList& out, Diagnostics& diags);
bool tokenIs(const char* src, const Token& t, const char* word);  // case-insensitive, NAME tokens only
void tokenText(const char* src, const Token& t, char* out, size_t cap);

}  // namespace logo
}  // namespace cq
