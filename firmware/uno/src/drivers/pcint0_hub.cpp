#include <Arduino.h>
#include <util/atomic.h>

#include "drivers/board_config.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

// PCINT0 serves D9 (PB1, IR) and D12 (PB4, echo). Capture only; < 10 µs.
namespace {
constexpr uint8_t kIrBit = _BV(PB1);
constexpr uint8_t kEchoBit = _BV(PB4);
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
}  // namespace

ISR(PCINT0_vect) {
  const uint32_t now = micros();
  const uint8_t pinb = PINB;
  const uint8_t changed = pinb ^ g_prev_pinb;
  g_prev_pinb = pinb;
  if ((changed & kIrBit) && !(pinb & kIrBit) && g_ir_count < kRing) {
    g_ir_ring[(g_ir_head + g_ir_count) % kRing] = now;
    ++g_ir_count;
  }
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

namespace cq {
namespace port {

namespace ir {
void begin() {
  pinMode(board::kPinIr, INPUT);
  g_prev_pinb = PINB;
  PCMSK0 |= _BV(PCINT1);
  PCICR |= _BV(PCIE0);
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
  PCMSK0 |= _BV(PCINT4);
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
