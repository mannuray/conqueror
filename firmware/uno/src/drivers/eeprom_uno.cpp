#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>

#include "ports/eeprom.h"

namespace cq {
namespace port {
namespace eeprom {

bool busy() { return (EECR & _BV(EEPE)) != 0; }

uint8_t read(uint16_t addr) { return eeprom_read_byte(reinterpret_cast<const uint8_t*>(addr)); }

bool writeByteAsync(uint16_t addr, uint8_t value) {
  if (busy()) return false;
  EEAR = addr;
  EEDR = value;
  const uint8_t sreg = SREG;
  cli();
  EECR |= _BV(EEMPE);
  EECR |= _BV(EEPE);
  SREG = sreg;
  return true;
}

}  // namespace eeprom
}  // namespace port
}  // namespace cq
