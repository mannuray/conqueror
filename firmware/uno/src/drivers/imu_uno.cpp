#include <Arduino.h>

#include "drivers/board_config.h"
#include "domain/estimation/imu_scale.h"
#include "ports/imu.h"

#include "drivers/soft_i2c.h"

namespace cq {
namespace port {
namespace imu {
namespace {

uint16_t g_errors = 0;

// ---- bus layer: software I2C on A4/A5 on both boards (Mega: LLD §11.4; Uno Lite: no Wire buffers, ~210 B RAM) ----
void busBegin() { drv::soft_i2c::begin(); }
bool busWrite(uint8_t reg, uint8_t value) { return drv::soft_i2c::writeReg(board::kImuAddress, reg, value); }
bool busRead(uint8_t reg, uint8_t* buf, uint8_t n) {
  return drv::soft_i2c::readRegs(board::kImuAddress, reg, buf, n);
}
void busRecover() { drv::soft_i2c::recover(); }
bool busTimedOut() { return true; }  // every soft-I2C failure gets the (cheap) recovery

void onFailure() {
  ++g_errors;
  if (busTimedOut()) busRecover();
}

bool writeReg(uint8_t reg, uint8_t value) {
  if (busWrite(reg, value)) return true;
  onFailure();
  return false;
}

int16_t pick(const int16_t v[3], int8_t map) {
  const int16_t x = v[(map < 0 ? -map : map) - 1];
  return map < 0 ? static_cast<int16_t>(-x) : x;
}

}  // namespace

bool begin() {
  busBegin();
  uint8_t who = 0;
  if (!busRead(0x75, &who, 1)) {  // WHO_AM_I
    onFailure();
    return false;
  }
  if (!imu_scale::supportedWhoAmI(who)) return false;
  return writeReg(0x6B, 0x01)     // PWR_MGMT_1: wake, PLL with gyro X
         && writeReg(0x19, 0x04)  // SMPLRT_DIV: 1 kHz / 5 = 200 Hz
         && writeReg(0x1A, 0x03)  // CONFIG: DLPF 44 Hz
         && writeReg(0x1B, 0x08)  // GYRO_CONFIG: ±500 °/s
         && writeReg(0x1C, 0x08); // ACCEL_CONFIG: ±4 g
}

bool read(ImuRaw& out) {
  uint8_t b[14];
  if (!busRead(0x3B, b, 14)) {  // ACCEL_XOUT_H .. GYRO_ZOUT_L
    onFailure();
    return false;
  }
  int16_t v[7];
  for (uint8_t i = 0; i < 7; ++i) v[i] = static_cast<int16_t>((b[2 * i] << 8) | b[2 * i + 1]);
  const int16_t acc[3] = {v[0], v[1], v[2]};
  const int16_t gyr[3] = {v[4], v[5], v[6]};
  out.ax = pick(acc, board::kImuMapX);
  out.ay = pick(acc, board::kImuMapY);
  out.az = pick(acc, board::kImuMapZ);
  out.gx = pick(gyr, board::kImuMapX);
  out.gy = pick(gyr, board::kImuMapY);
  out.gz = pick(gyr, board::kImuMapZ);
  out.temp = v[3];
  out.t_us = micros();
  return true;
}

uint16_t i2cErrors() { return g_errors; }

}  // namespace imu
}  // namespace port
}  // namespace cq
