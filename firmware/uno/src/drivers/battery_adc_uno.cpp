#include <avr/io.h>

#include "ports/battery_adc.h"

namespace cq {
namespace port {
namespace battery_adc {

void begin() { ADMUX = _BV(REFS0) | 3; }  // AVcc reference, channel A3 (core already set ADEN, /128)

void start() {
  ADMUX = _BV(REFS0) | 3;
  ADCSRA |= _BV(ADSC);
}

bool ready(uint16_t& adc) {
  if (ADCSRA & _BV(ADSC)) return false;
  adc = ADC;
  return true;
}

}  // namespace battery_adc
}  // namespace port
}  // namespace cq
