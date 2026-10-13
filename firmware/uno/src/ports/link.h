#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace link {
void begin(uint32_t baud);
bool readByte(uint8_t& b);
bool write(const uint8_t* data, uint8_t len);  // false = no room for the whole frame
uint8_t txFree();
}  // namespace link
}  // namespace port
}  // namespace cq
