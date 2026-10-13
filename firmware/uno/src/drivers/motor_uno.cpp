#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/motor.h"

namespace cq {
namespace port {
namespace motor {
namespace {

void side(uint8_t pwm_pin, uint8_t dir_pin, bool fwd_high, int16_t value) {
  const bool forward = value >= 0;
  digitalWrite(dir_pin, forward == fwd_high ? HIGH : LOW);
  analogWrite(pwm_pin, static_cast<uint8_t>(forward ? value : -value));
}

}  // namespace

void begin() {
  pinMode(board::kPinPwmLeft, OUTPUT);
  pinMode(board::kPinPwmRight, OUTPUT);
  pinMode(board::kPinDirLeft, OUTPUT);
  pinMode(board::kPinDirRight, OUTPUT);
  analogWrite(board::kPinPwmLeft, 0);
  analogWrite(board::kPinPwmRight, 0);
  if (board::kHasStandby) {
    pinMode(board::kPinStandby, OUTPUT);
    digitalWrite(board::kPinStandby, LOW);
  }
}

void apply(const PwmOutput& out) {
  if (board::kHasStandby) digitalWrite(board::kPinStandby, out.enabled() ? HIGH : LOW);
  const int16_t l = out.enabled() ? out.left() : 0;
  const int16_t r = out.enabled() ? out.right() : 0;
  side(board::kPinPwmLeft, board::kPinDirLeft, board::kDirLeftForwardHigh, l);
  side(board::kPinPwmRight, board::kPinDirRight, board::kDirRightForwardHigh, r);
}

}  // namespace motor
}  // namespace port
}  // namespace cq
