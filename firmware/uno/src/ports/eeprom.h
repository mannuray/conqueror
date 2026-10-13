#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace eeprom {
uint8_t read(uint16_t addr);                       // waits for a running write (only at boot / idle)
bool writeByteAsync(uint16_t addr, uint8_t value);  // false if a write is running
bool busy();
}  // namespace eeprom
}  // namespace port
}  // namespace cq
