// Serial1 to the Uno: RX GPIO3, TX GPIO40, 250000 8N1 (bridge LLD R2.1). Only link_task uses it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

class UartPort {
 public:
  void begin();
  size_t read(uint8_t* buf, size_t max);
  size_t txFree();
  void write(const uint8_t* data, size_t len);  // caller checked txFree() >= len
};

}  // namespace net
}  // namespace cq
