#include <Arduino.h>
#include <Servo.h>

#include "drivers/board_config.h"
#include "ports/servos.h"

namespace cq {
namespace port {
namespace servos {
namespace {
Servo g_servo[2];
const uint8_t kPins[2] = {board::kPinServoPan, board::kPinServoTilt};
}  // namespace

void begin() {}

void attach(Axis a) {
  Servo& s = g_servo[static_cast<uint8_t>(a)];
  if (!s.attached()) s.attach(kPins[static_cast<uint8_t>(a)], 544, 2400);
}

void detach(Axis a) {
  Servo& s = g_servo[static_cast<uint8_t>(a)];
  if (s.attached()) s.detach();
}

void writeDeg(Axis a, float deg) {
  if (deg < 0.0f) deg = 0.0f;
  if (deg > 180.0f) deg = 180.0f;
  g_servo[static_cast<uint8_t>(a)].writeMicroseconds(static_cast<int>(544.0f + deg * (1856.0f / 180.0f)));
}

}  // namespace servos
}  // namespace port
}  // namespace cq
