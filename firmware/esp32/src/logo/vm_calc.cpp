#include "logo/vm_calc.h"

#include <stddef.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

constexpr const char* kSymbols[] = {"?", "+", "-", "*", "/", "=", "<>", "<", ">", "<=", ">=", "AND", "OR", "NOT", "-"};
static_assert(sizeof kSymbols / sizeof kSymbols[0] == static_cast<size_t>(Op::kNeg) + 1, "one per Op");

CalcErr fit(int64_t v, int32_t& out) {
  if (v > gen::kNumberAbsMaxMilli || v < -static_cast<int64_t>(gen::kNumberAbsMaxMilli)) return CalcErr::kTooBig;
  out = static_cast<int32_t>(v);
  return CalcErr::kNone;
}

}  // namespace

CalcErr arith(Op op, int32_t a, int32_t b, int32_t& out) {
  const int64_t x = a, y = b;
  switch (op) {
    case Op::kAdd: return fit(x + y, out);
    case Op::kSub: return fit(x - y, out);
    case Op::kMul: return fit(x * y / 1000, out);
    case Op::kDiv: return y == 0 ? CalcErr::kDivZero : fit(x * 1000 / y, out);
    default: return fit(0, out);
  }
}

bool compare(Op op, int32_t a, int32_t b) {
  switch (op) {
    case Op::kLt: return a < b;
    case Op::kGt: return a > b;
    case Op::kLe: return a <= b;
    case Op::kGe: return a >= b;
    case Op::kNe: return a != b;
    default: return a == b;
  }
}

CalcErr remainder(int32_t a, int32_t b, int32_t& out) {
  if (b == 0) return CalcErr::kDivZero;
  out = a % b;
  return CalcErr::kNone;
}

int32_t roundWhole(int32_t x) {
  const int32_t mag = ((x < 0 ? -x : x) + 500) / 1000 * 1000;
  return x < 0 ? -mag : mag;
}

const char* opSymbol(Op op) { return kSymbols[static_cast<uint8_t>(op)]; }

}  // namespace logo
}  // namespace cq
