#if defined(BOARD_MEGA)
#include "drivers/i2c_bus.h"

#include <avr/io.h>
#include <util/delay.h>

namespace {
constexpr uint8_t kSda = _BV(PF4);  // A4
constexpr uint8_t kScl = _BV(PF5);  // A5

inline void halfBit() { _delay_us(1); }
inline void sdaLow() { DDRF |= kSda; }
inline void sdaRelease() { DDRF &= static_cast<uint8_t>(~kSda); }
inline void sclLow() { DDRF |= kScl; }
inline bool sdaHigh() { return (PINF & kSda) != 0; }

// Releases SCL and waits (≤ 1 ms) for a slave that stretches the clock.
bool sclRelease() {
  DDRF &= static_cast<uint8_t>(~kScl);
  for (uint16_t i = 0; i < 500; ++i) {
    if (PINF & kScl) return true;
    _delay_us(2);
  }
  return false;
}

bool start() {  // also a repeated START: SDA high while SCL low, then SCL high, SDA falls
  sdaRelease();
  halfBit();
  if (!sclRelease()) return false;
  halfBit();
  sdaLow();
  halfBit();
  sclLow();
  halfBit();
  return true;
}

void stop() {
  sdaLow();
  halfBit();
  sclRelease();
  halfBit();
  sdaRelease();
  halfBit();
}

bool writeByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; ++i) {
    if (b & 0x80) {
      sdaRelease();
    } else {
      sdaLow();
    }
    halfBit();
    if (!sclRelease()) return false;
    halfBit();
    sclLow();
    b = static_cast<uint8_t>(b << 1);
  }
  sdaRelease();  // ACK bit from the slave
  halfBit();
  if (!sclRelease()) return false;
  const bool ack = !sdaHigh();
  halfBit();
  sclLow();
  return ack;
}

bool readByte(uint8_t& out, bool ack) {
  sdaRelease();
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    halfBit();
    if (!sclRelease()) return false;
    v = static_cast<uint8_t>((v << 1) | (sdaHigh() ? 1 : 0));
    halfBit();
    sclLow();
  }
  if (ack) {
    sdaLow();
  } else {
    sdaRelease();
  }
  halfBit();
  if (!sclRelease()) return false;
  halfBit();
  sclLow();
  sdaRelease();
  out = v;
  return true;
}
}  // namespace

namespace cq {
namespace drv {
namespace i2c {

void begin() {
  PORTF &= static_cast<uint8_t>(~(kSda | kScl));  // when driven, drive LOW; never source HIGH
  DDRF &= static_cast<uint8_t>(~(kSda | kScl));   // both released
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  const bool ok = start() && writeByte(static_cast<uint8_t>(addr << 1)) && writeByte(reg) && writeByte(value);
  stop();
  return ok;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n) {
  bool ok = start() && writeByte(static_cast<uint8_t>(addr << 1)) && writeByte(reg) && start() &&
            writeByte(static_cast<uint8_t>((addr << 1) | 1));
  for (uint8_t i = 0; ok && i < n; ++i) ok = readByte(buf[i], i + 1 < n);
  stop();
  return ok;
}

void recover() {
  sdaRelease();
  for (uint8_t i = 0; i < 9 && !sdaHigh(); ++i) {
    sclLow();
    _delay_us(5);
    sclRelease();
    _delay_us(5);
  }
  stop();
}

}  // namespace i2c
}  // namespace drv
}  // namespace cq
#endif  // BOARD_MEGA
