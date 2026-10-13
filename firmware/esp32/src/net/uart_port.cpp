#include "net/uart_port.h"

#include <Arduino.h>

#include "board_config.h"

namespace cq {
namespace net {

void UartPort::begin() {
  Serial1.setRxBufferSize(board::kUartRxBufSize);
  Serial1.setTxBufferSize(board::kUartTxBufSize);
  Serial1.begin(board::kUnoBaud, SERIAL_8N1, board::kUnoRxPin, board::kUnoTxPin);
}

size_t UartPort::read(uint8_t* buf, size_t max) {
  const int avail = Serial1.available();
  if (avail <= 0) return 0;
  return Serial1.read(buf, static_cast<size_t>(avail) < max ? static_cast<size_t>(avail) : max);
}

size_t UartPort::txFree() { return static_cast<size_t>(Serial1.availableForWrite()); }

void UartPort::write(const uint8_t* data, size_t len) { Serial1.write(data, len); }

}  // namespace net
}  // namespace cq
