// Tank Logo arithmetic on 1/1000 fixed point (LLD §7), shared by the Vm's operators and built-in reporters.
// Internal to src/logo.
#pragma once

#include <stdint.h>

#include "logo/ast.h"

namespace cq {
namespace logo {

enum class CalcErr : uint8_t { kNone, kDivZero, kTooBig };

CalcErr arith(Op op, int32_t a, int32_t b, int32_t& out);  // + - * / on numbers
bool compare(Op op, int32_t a, int32_t b);                 // < > <= >= = <>
CalcErr remainder(int32_t a, int32_t b, int32_t& out);     // sign of a (REMAINDER -7 2 = -1)
int32_t roundWhole(int32_t x);                             // halves away from zero
const char* opSymbol(Op op);                               // "+" … for messages

}  // namespace logo
}  // namespace cq
