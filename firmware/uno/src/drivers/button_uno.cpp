#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/button.h"

namespace cq {
namespace port {
namespace button {
void begin() { pinMode(board::kPinButton, INPUT_PULLUP); }
bool pressed() { return digitalRead(board::kPinButton) == LOW; }
}  // namespace button
}  // namespace port
}  // namespace cq
