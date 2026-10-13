#include <Arduino.h>

#include "ports/link.h"

namespace cq {
namespace port {
namespace link {

void begin(uint32_t baud) { Serial.begin(baud); }

bool readByte(uint8_t& b) {
  const int v = Serial.read();
  if (v < 0) return false;
  b = static_cast<uint8_t>(v);
  return true;
}

uint8_t txFree() {
  const int n = Serial.availableForWrite();
  return n > 255 ? 255 : static_cast<uint8_t>(n);
}

bool write(const uint8_t* data, uint8_t len) {
  if (txFree() < len) return false;
  Serial.write(data, len);
  return true;
}

}  // namespace link
}  // namespace port
}  // namespace cq
