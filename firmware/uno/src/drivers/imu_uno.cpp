#include <Arduino.h>

#include "drivers/board_config.h"
#include "domain/estimation/imu_scale.h"
#include "ports/imu.h"

#if defined(BOARD_MEGA)
#include "drivers/soft_i2c.h"
#else
#include <Wire.h>
#endif

namespace cq {
namespace port {
namespace imu {
namespace {

uint16_t g_errors = 0;

// ---- bus layer: hardware TWI on the Uno, software I2C on A4/A5 on the Mega (LLD §11.4) ----
#if defined(BOARD_MEGA)
void busBegin() { drv::soft_i2c::begin(); }
bool busWrite(uint8_t reg, uint8_t value) { return drv::soft_i2c::writeReg(board::kImuAddress, reg, value); }
bool busRead(uint8_t reg, uint8_t* buf, uint8_t n) {
  return drv::soft_i2c::readRegs(board::kImuAddress, reg, buf, n);
}
void busRecover() { drv::soft_i2c::recover(); }
bool busTimedOut() { return true; }  // every soft-I2C failure gets the (cheap) recovery
#else
void busBegin() {
  Wire.begin();
  Wire.setClock(400000UL);
  Wire.setWireTimeout(3000, true);  // 3 ms; resets the TWI on a stuck bus
}

bool busWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool busRead(uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(board::kImuAddress, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

// I2C bus recovery (LLD §9.13): a slave stuck mid-byte holds SDA low; clock SCL up to 9 times
// until it lets go, then send a STOP and restart the TWI. Rare (only after a Wire timeout);
// ~100 µs of 5 µs busy-waits, a documented exception like the burst read.
void busRecover() {
  Wire.end();
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, OUTPUT);
  for (uint8_t i = 0; i < 9 && digitalRead(SDA) == LOW; ++i) {
    digitalWrite(SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(SDA, OUTPUT);  // STOP: SDA low → high while SCL is high
  digitalWrite(SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(SCL, HIGH);
  delayMicroseconds(5);
  pinMode(SDA, INPUT_PULLUP);
  busBegin();
}

bool busTimedOut() {
  if (!Wire.getWireTimeoutFlag()) return false;
  Wire.clearWireTimeoutFlag();
  return true;
}
#endif

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
