#pragma once
#include <stdint.h>

namespace cq {
namespace ir_codes {
// ELEGOO remote, NEC, MSB-first as received. Values from the original ELEGOO firmware constants;
// LLD §9.8 listed 5, 8 and 9 wrongly (fixed in Task 15).
constexpr uint32_t kUp = 0x00FF629DUL;
constexpr uint32_t kDown = 0x00FFA857UL;
constexpr uint32_t kLeft = 0x00FF22DDUL;
constexpr uint32_t kRight = 0x00FFC23DUL;
constexpr uint32_t kOk = 0x00FF02FDUL;
constexpr uint32_t k1 = 0x00FF6897UL;
constexpr uint32_t k2 = 0x00FF9867UL;
constexpr uint32_t k3 = 0x00FFB04FUL;
constexpr uint32_t k4 = 0x00FF30CFUL;
constexpr uint32_t k5 = 0x00FF18E7UL;
constexpr uint32_t k6 = 0x00FF7A85UL;
constexpr uint32_t k7 = 0x00FF10EFUL;
constexpr uint32_t k8 = 0x00FF38C7UL;
constexpr uint32_t k9 = 0x00FF5AA5UL;
constexpr uint32_t k0 = 0x00FF4AB5UL;
constexpr uint32_t kStar = 0x00FF42BDUL;
constexpr uint32_t kHash = 0x00FF52ADUL;
}  // namespace ir_codes
}  // namespace cq
