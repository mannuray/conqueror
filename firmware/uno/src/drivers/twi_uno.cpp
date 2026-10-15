#if !defined(BOARD_MEGA)
#include <avr/io.h>
#include <util/delay.h>
#include <util/twi.h>

#include "drivers/i2c_bus.h"

namespace {
// TWINT normally sets within ~25 µs per byte at 400 kHz; ~3 cycles per loop → ~0.4 ms before giving up
// (a stuck bus fails the transfer instead of hanging the loop; the IMU task then counts the failure).
constexpr uint16_t kSpinMax = 2000;
constexpr uint8_t kSda = _BV(PC4);  // A4
constexpr uint8_t kScl = _BV(PC5);  // A5

bool waitInt() {
  for (uint16_t i = 0; i < kSpinMax; ++i) {
    if (TWCR & _BV(TWINT)) return true;
  }
  return false;
}

bool start() {
  TWCR = _BV(TWINT) | _BV(TWSTA) | _BV(TWEN);
  if (!waitInt()) return false;
  const uint8_t st = TW_STATUS;
  return st == TW_START || st == TW_REP_START;
}

void stop() {
  TWCR = _BV(TWINT) | _BV(TWSTO) | _BV(TWEN);
  for (uint16_t i = 0; i < kSpinMax && (TWCR & _BV(TWSTO)); ++i) {
  }
}

bool send(uint8_t b, uint8_t expect) {
  TWDR = b;
  TWCR = _BV(TWINT) | _BV(TWEN);
  return waitInt() && TW_STATUS == expect;
}

bool receive(uint8_t& b, bool ack) {
  TWCR = static_cast<uint8_t>(_BV(TWINT) | _BV(TWEN) | (ack ? _BV(TWEA) : 0));
  if (!waitInt()) return false;
  b = TWDR;
  return TW_STATUS == (ack ? TW_MR_DATA_ACK : TW_MR_DATA_NACK);
}
}  // namespace

namespace cq {
namespace drv {
namespace i2c {

void begin() {
  DDRC &= static_cast<uint8_t>(~(kSda | kScl));
  PORTC |= kSda | kScl;  // internal pull-ups as the Wire library did (the IMU module also has its own)
  TWSR = 0;              // prescaler 1
  TWBR = static_cast<uint8_t>((F_CPU / 400000UL - 16UL) / 2UL);  // 400 kHz
  TWCR = _BV(TWEN);
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  const bool ok = start() && send(static_cast<uint8_t>(addr << 1), TW_MT_SLA_ACK) && send(reg, TW_MT_DATA_ACK) &&
                  send(value, TW_MT_DATA_ACK);
  stop();
  return ok;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n) {
  bool ok = start() && send(static_cast<uint8_t>(addr << 1), TW_MT_SLA_ACK) && send(reg, TW_MT_DATA_ACK) && start() &&
            send(static_cast<uint8_t>((addr << 1) | 1u), TW_MR_SLA_ACK);
  for (uint8_t i = 0; ok && i < n; ++i) ok = receive(buf[i], i + 1u < n);
  stop();
  return ok;
}

// A slave stuck mid-byte holds SDA low: clock SCL as GPIO up to 9 times until it lets go, then restart the TWI
// (LLD §9.13; ~100 µs of 5 µs busy-waits, a documented exception like the burst read).
void recover() {
  TWCR = 0;
  PORTC &= static_cast<uint8_t>(~kScl);
  for (uint8_t i = 0; i < 9 && !(PINC & kSda); ++i) {
    DDRC |= kScl;
    _delay_us(5);
    DDRC &= static_cast<uint8_t>(~kScl);
    _delay_us(5);
  }
  begin();
}

}  // namespace i2c
}  // namespace drv
}  // namespace cq
#endif  // !BOARD_MEGA
