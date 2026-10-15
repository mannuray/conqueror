#pragma once
#include <stdint.h>

// I2C master for the IMU on A4 (SDA) / A5 (SCL). Two implementations, one per board:
// - Mega: software I2C (soft_i2c.cpp) - A4/A5 are plain GPIO there (LLD §11.4), open-drain by toggling DDR, ~200 kHz.
// - Uno:  the ATmega328P's hardware TWI (twi_uno.cpp) - A4/A5 are the TWI pins; register-level, polled, 400 kHz,
//         no Arduino Wire library (its five 32-byte buffers do not fit the Lite RAM budget, uno-lite LLD §7).
// Both block for one transfer (≈ 0.35–0.6 ms for the 14-byte IMU burst): a documented exception like the full
// firmware's burst read.
namespace cq {
namespace drv {
namespace i2c {
void begin();
bool writeReg(uint8_t addr, uint8_t reg, uint8_t value);
bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n);
void recover();  // 9 SCL clocks while SDA is held low, then STOP (LLD §9.13)
}  // namespace i2c
}  // namespace drv
}  // namespace cq
