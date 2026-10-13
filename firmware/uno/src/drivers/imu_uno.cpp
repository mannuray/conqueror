#include <Arduino.h>
#include <Wire.h>

#include "drivers/board_config.h"
#include "ports/imu.h"

namespace cq {
namespace port {
namespace imu {
namespace {

uint16_t g_errors = 0;

void configureWire() {
  Wire.begin();
  Wire.setClock(400000UL);
  Wire.setWireTimeout(3000, true);  // 3 ms; resets the TWI on a stuck bus
}

// I2C bus recovery (LLD §9.13): a slave stuck mid-byte holds SDA low; clock SCL up to 9 times
// until it lets go, then send a STOP and restart the TWI. Rare (only after a Wire timeout);
// ~100 µs of 5 µs busy-waits, a documented exception like the burst read.
void recoverBus() {
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
  configureWire();
}

void onFailure() {
  ++g_errors;
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
    recoverBus();
  }
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) {
    onFailure();
    return false;
  }
  return true;
}

int16_t pick(const int16_t v[3], int8_t map) {
  const int16_t x = v[(map < 0 ? -map : map) - 1];
  return map < 0 ? static_cast<int16_t>(-x) : x;
}

}  // namespace

bool begin() {
  configureWire();
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(0x75);  // WHO_AM_I
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(board::kImuAddress, static_cast<uint8_t>(1)) != 1) {
    onFailure();
    return false;
  }
  if (Wire.read() != 0x68) return false;
  return writeReg(0x6B, 0x01)     // PWR_MGMT_1: wake, PLL with gyro X
         && writeReg(0x19, 0x04)  // SMPLRT_DIV: 1 kHz / 5 = 200 Hz
         && writeReg(0x1A, 0x03)  // CONFIG: DLPF 44 Hz
         && writeReg(0x1B, 0x08)  // GYRO_CONFIG: ±500 °/s
         && writeReg(0x1C, 0x08); // ACCEL_CONFIG: ±4 g
}

bool read(ImuRaw& out) {
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(0x3B);  // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(board::kImuAddress, static_cast<uint8_t>(14)) != 14) {
    onFailure();
    return false;
  }
  int16_t v[7];
  for (uint8_t i = 0; i < 7; ++i) {
    const uint8_t hi = static_cast<uint8_t>(Wire.read());
    const uint8_t lo = static_cast<uint8_t>(Wire.read());
    v[i] = static_cast<int16_t>((hi << 8) | lo);
  }
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
