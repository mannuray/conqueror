// The `reads` notation of logo/spec/tests/README.md, for the conformance tests (LLD §17). No recursion.
#pragma once

#include <stddef.h>

#include "logo/ast.h"
#include "logo/lexer.h"
#include "logo/parser.h"

namespace cq {
namespace logo {

void printReads(const Node* program, const char* src, const TokenList& toks, const Parser& p, char* out, size_t cap);

}  // namespace logo
}  // namespace cq
