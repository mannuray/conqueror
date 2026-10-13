#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/tone.h"

namespace cq {
namespace port {
namespace tone {
void begin() { pinMode(board::kPinBuzzer, OUTPUT); }
void play(uint16_t hz) { ::tone(board::kPinBuzzer, hz); }
void stop() { ::noTone(board::kPinBuzzer); }
}  // namespace tone
}  // namespace port
}  // namespace cq
