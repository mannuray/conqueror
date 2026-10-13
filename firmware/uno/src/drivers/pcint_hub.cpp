#include <Arduino.h>
#include <util/atomic.h>

#include "drivers/board_config.h"
#include "drivers/pcint_hub.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

// Capture only in every ISR; < 10 µs (LLD §9.15, §11.2).
namespace {
#if defined(BOARD_MEGA)
constexpr uint8_t kEchoBit = _BV(PB6);   // D12
constexpr uint8_t kIrBit = _BV(PK6);     // A14 (PCINT2 group)
constexpr uint8_t kEncLeftBit = _BV(PK0);   // A8
constexpr uint8_t kEncRightBit = _BV(PK2);  // A10
volatile uint8_t g_prev_pink = 0xFF;
#else
constexpr uint8_t kIrBit = _BV(PB1);     // D9 (PCINT0 group)
constexpr uint8_t kEchoBit = _BV(PB4);   // D12
constexpr uint8_t kEncLeftBit = _BV(PC1);
constexpr uint8_t kEncRightBit = _BV(PC2);
volatile uint8_t g_prev_pinc = 0;
#endif
constexpr uint8_t kRing = 16;

volatile uint8_t g_prev_pinb = 0xFF;
volatile uint32_t g_ir_ring[kRing];
volatile uint8_t g_ir_head = 0;
volatile uint8_t g_ir_count = 0;
volatile uint32_t g_echo_rise_us = 0;
volatile uint32_t g_echo_fall_us = 0;
volatile bool g_echo_rose = false;
volatile bool g_echo_fell = false;
uint32_t g_trig_us = 0;  // main context only
bool g_armed = false;    // the last trigger() sent a ping

inline void irEdge(uint8_t changed, uint8_t pins, uint32_t now) {
  if ((changed & kIrBit) && !(pins & kIrBit) && g_ir_count < kRing) {
    g_ir_ring[(g_ir_head + g_ir_count) % kRing] = now;
    ++g_ir_count;
  }
}

inline void echoEdge(uint8_t changed, uint8_t pinb, uint32_t now) {
  if (changed & kEchoBit) {
    if (pinb & kEchoBit) {
      g_echo_rise_us = now;
      g_echo_rose = true;
    } else if (g_echo_rose) {
      g_echo_fall_us = now;
      g_echo_fell = true;
    }
  }
}
}  // namespace

// Weak stand-in until the encoder driver (Task 7) provides the real capture.
__attribute__((weak)) void cq::drv::encoderEdges(bool, bool, uint32_t) {}

ISR(PCINT0_vect) {
  const uint32_t now = micros();
  const uint8_t pinb = PINB;
  const uint8_t changed = pinb ^ g_prev_pinb;
  g_prev_pinb = pinb;
#if !defined(BOARD_MEGA)
  irEdge(changed, pinb, now);
#endif
  echoEdge(changed, pinb, now);
}

#if defined(BOARD_MEGA)
ISR(PCINT2_vect) {
  const uint32_t now = micros();
  const uint8_t pink = PINK;
  const uint8_t changed = pink ^ g_prev_pink;
  g_prev_pink = pink;
  irEdge(changed, pink, now);
  const uint8_t rising = changed & pink;
  if (rising & (kEncLeftBit | kEncRightBit)) {
    cq::drv::encoderEdges(rising & kEncLeftBit, rising & kEncRightBit, now);
  }
}
#else
ISR(PCINT1_vect) {
  const uint32_t now = micros();
  const uint8_t pinc = PINC;
  const uint8_t rising = static_cast<uint8_t>((pinc ^ g_prev_pinc) & pinc);
  g_prev_pinc = pinc;
  if (rising & (kEncLeftBit | kEncRightBit)) {
    cq::drv::encoderEdges(rising & kEncLeftBit, rising & kEncRightBit, now);
  }
}
#endif

namespace cq {
namespace port {

namespace ir {
void begin() {
#if defined(BOARD_MEGA)
  pinMode(board::kPinIrShield, INPUT);  // jumpered to A14; never driven
  pinMode(board::kPinIr, INPUT);
  g_prev_pink = PINK;
  PCMSK2 |= _BV(PCINT22);
  PCICR |= _BV(PCIE2);
#else
  pinMode(board::kPinIr, INPUT);
  g_prev_pinb = PINB;
  PCMSK0 |= _BV(PCINT1);
  PCICR |= _BV(PCIE0);
#endif
}

bool popEdge(uint32_t& t_us) {
  bool ok = false;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (g_ir_count > 0) {
      t_us = g_ir_ring[g_ir_head];
      g_ir_head = static_cast<uint8_t>((g_ir_head + 1) % kRing);
      --g_ir_count;
      ok = true;
    }
  }
  return ok;
}
}  // namespace ir

namespace ultrasonic {
void begin() {
  pinMode(board::kPinTrig, OUTPUT);
  digitalWrite(board::kPinTrig, LOW);
  // Pull-up: HC-SR04 echo is push-pull, so harmless when connected; unplugged it reads a steady
  // HIGH (no edges, no PCINT storm) → kNoEcho → US_FAULT.
  pinMode(board::kPinEcho, INPUT_PULLUP);
  g_prev_pinb = PINB;
#if defined(BOARD_MEGA)
  PCMSK0 |= _BV(PCINT6);
#else
  PCMSK0 |= _BV(PCINT4);
#endif
  PCICR |= _BV(PCIE0);
}

bool trigger() {
  // LLD §9.16: no new ping while the echo is still high (clone modules hold it up to 200 ms).
  if (PINB & kEchoBit) {
    g_armed = false;
    return false;
  }
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    g_echo_rose = false;
    g_echo_fell = false;
  }
  digitalWrite(board::kPinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(board::kPinTrig, LOW);
  g_trig_us = micros();
  g_armed = true;
  return true;
}

EchoState poll(uint16_t& echo_us) {
  uint32_t rise = 0, fall = 0;
  bool rose = false, fell = false;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    rise = g_echo_rise_us;
    fall = g_echo_fall_us;
    rose = g_echo_rose;
    fell = g_echo_fell;
  }
  if (!g_armed) return EchoState::kPending;  // previous trigger() sent no ping
  // No rising edge within 2 ms of the trigger → the sensor did not answer (a failed measurement).
  if (!rose || static_cast<uint32_t>(rise - g_trig_us) > 2000UL) return EchoState::kNoEcho;
  if (!fell) return EchoState::kClear;    // echo still high after 50 ms: nothing in range
  const uint32_t w = fall - rise;
  if (w > 25000UL) return EchoState::kClear;  // LLD §9.16: echo high > 25 ms = nothing in range
  echo_us = static_cast<uint16_t>(w);
  return EchoState::kReady;
}
}  // namespace ultrasonic

}  // namespace port
}  // namespace cq

void cq::drv::encodersEnablePcint() {
  pinMode(cq::board::kPinEncLeft, INPUT_PULLUP);
  pinMode(cq::board::kPinEncRight, INPUT_PULLUP);
#if defined(BOARD_MEGA)
  g_prev_pink = PINK;
  PCMSK2 |= _BV(PCINT16) | _BV(PCINT18);
  PCICR |= _BV(PCIE2);
#else
  g_prev_pinc = PINC;
  PCMSK1 |= _BV(PCINT9) | _BV(PCINT10);
  PCICR |= _BV(PCIE1);
#endif
}
