#include "net/log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "board_config.h"

namespace cq {
namespace net {

void logBegin() {
  Serial.setTxBufferSize(2048);  // room for bursts: lines are dropped only when nobody reads
  Serial.begin(board::kLogBaud);
  // Never block: with the USB cable in a computer that does not read the port, blocking writes
  // delayed link_task / httpd (slower WS replies measured on the board 2026-10-13).
  Serial.setTxTimeoutMs(0);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {
    vTaskDelay(pdMS_TO_TICKS(10));  // wait up to 2 s for a USB host (boot log), never block forever
  }
}

void logf(const char* fmt, ...) {
  char line[224];
  int n = snprintf(line, sizeof(line), "[%7lu] ", static_cast<unsigned long>(millis()));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n) - 2, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\r';
  line[len++] = '\n';
  if (Serial.availableForWrite() < static_cast<int>(len)) return;  // nobody reading: drop the line
  Serial.write(reinterpret_cast<const uint8_t*>(line), len);  // one write: lines never mix
}

}  // namespace net
}  // namespace cq
