#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/led.h"

static_assert(cq::board::kPinLed == 4, "ws2812_uno.cpp drives D4 directly");
// D4 is PD4 on the Uno and PG5 on the Mega (LLD §11.1). Both ports are in the low I/O space, so the
// cycle-counted sbi/cbi timing is identical on both boards.
#if defined(BOARD_MEGA)
#define kLedPortReg PORTG
#define kLedPortBit PORTG5
#else
#define kLedPortReg PORTD
#define kLedPortBit PORTD4
#endif

static_assert(F_CPU == 16000000UL, "WS2812 timing is written for 16 MHz");

namespace cq {
namespace port {
namespace led {
namespace {

void sendByte(uint8_t byte) {
  uint8_t bits = 8;
  asm volatile(
      "1:                       \n\t"
      "  sbi  %[port], %[pin]   \n\t"  // 2  rise
      "  rjmp .+0               \n\t"  // 2
      "  sbrs %[byte], 7        \n\t"  // 1 (bit 0) / 2 (bit 1, skips the cbi)
      "  cbi  %[port], %[pin]   \n\t"  // 2  0-bit falls here (~6 cycles high)
      "  rjmp .+0               \n\t"  // 2
      "  rjmp .+0               \n\t"  // 2
      "  lsl  %[byte]           \n\t"  // 1
      "  cbi  %[port], %[pin]   \n\t"  // 2  1-bit falls here (~13 cycles high)
      "  dec  %[bits]           \n\t"  // 1
      "  rjmp .+0               \n\t"  // 2
      "  rjmp .+0               \n\t"  // 2  T1L ≈ 7 cycles (440 ns)
      "  brne 1b                \n\t"  // 2
      : [byte] "+r"(byte), [bits] "+r"(bits)
      : [port] "I"(_SFR_IO_ADDR(kLedPortReg)), [pin] "I"(kLedPortBit));
}

}  // namespace

void begin() {
  pinMode(board::kPinLed, OUTPUT);
  digitalWrite(board::kPinLed, LOW);
}

void write(const Rgb& c) {
  const uint8_t sreg = SREG;
  cli();
  sendByte(c.g);  // WS2812 order: G, R, B, MSB first
  sendByte(c.r);
  sendByte(c.b);
  SREG = sreg;
}

}  // namespace led
}  // namespace port
}  // namespace cq
