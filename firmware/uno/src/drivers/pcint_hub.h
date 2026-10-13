#pragma once
#include <stdint.h>

// Pin-change interrupt hub (drivers only; LLD §11.2). All PCINT ISRs live in pcint_hub.cpp:
//   Uno : PCINT0 = IR (PB1) + echo (PB4); PCINT1 = encoders (PC1/PC2)
//   Mega: PCINT0 = echo (PB6);            PCINT2 = IR (PK6) + encoders (PK0 left, PK2 right)
// Encoder capture is a hook the encoder driver implements (Task 7); a weak no-op stands in before.
namespace cq {
namespace drv {
void encoderEdges(bool left_rising, bool right_rising, uint32_t now_us);  // called from the ISR
void encodersEnablePcint();  // pull-ups + pin-change mask for both encoder pins
}  // namespace drv
}  // namespace cq
