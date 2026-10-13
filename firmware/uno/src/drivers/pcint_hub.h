#pragma once
#include <stdint.h>

// Pin-change interrupt hub (drivers only; LLD §11.2). All PCINT ISRs live in pcint_hub.cpp:
//   Uno : PCINT0 = IR (PB1) + echo (PB4); PCINT1 = encoders (PC1/PC2)
//   Mega: PCINT0 = echo (PB6);            PCINT2 = encoders (PK0 left, PK2 right);
//         IR on D9 (PH6) is sampled by Timer1 at 20 kHz (LLD §11.4)
// Encoder capture is a hook the encoder driver implements (Task 7); a weak no-op stands in before.
namespace cq {
namespace drv {
void encoderEdges(bool left_rising, bool right_rising, uint32_t now_us);  // called from the ISR
void encodersEnablePcint();  // pull-ups + pin-change mask for both encoder pins
}  // namespace drv
}  // namespace cq
