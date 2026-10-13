#pragma once
#include <stdint.h>

// Software I2C master on A4 (SDA) / A5 (SCL) for the Mega (LLD §11.4): the shield wires the
// MPU6050 to A4/A5, which are the Uno's TWI pins but plain GPIO on the Mega. Open-drain by
// toggling DDR (line released = input, pulled up by the IMU module; LOW = output 0). ~200 kHz.
namespace cq {
namespace drv {
namespace soft_i2c {
void begin();
bool writeReg(uint8_t addr, uint8_t reg, uint8_t value);
bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n);
void recover();  // 9 SCL clocks while SDA is held low, then STOP (LLD §9.13)
}  // namespace soft_i2c
}  // namespace drv
}  // namespace cq
