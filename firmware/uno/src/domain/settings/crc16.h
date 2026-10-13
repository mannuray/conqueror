#pragma once
#include <stdint.h>

namespace cq {

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF); check("123456789") = 0x29B1.
inline uint16_t crc16(const uint8_t* data, uint16_t len, uint16_t crc = 0xFFFF) {
  for (uint16_t i = 0; i < len; ++i) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace cq
