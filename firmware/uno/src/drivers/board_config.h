#pragma once
#include <stdint.h>

#if !defined(BOARD_UNO) && !defined(BOARD_MEGA)
#error "select a board: -DBOARD_MEGA or -DBOARD_UNO"
#endif

namespace cq {
namespace board {

#if defined(SHIELD_SMARTCAR_V11_TB6612)
constexpr uint8_t kPinPwmRight = 5;   // PWMA
constexpr uint8_t kPinPwmLeft = 6;    // PWMB
constexpr uint8_t kPinDirRight = 7;   // AIN1
constexpr uint8_t kPinDirLeft = 8;    // BIN1
constexpr uint8_t kPinStandby = 3;    // STBY, HIGH = enabled
constexpr bool kHasStandby = true;
#elif defined(SHIELD_TANK_DRV8835)
constexpr uint8_t kPinPwmRight = 5;   // AIN2/EN
constexpr uint8_t kPinPwmLeft = 6;    // BIN2/EN
constexpr uint8_t kPinDirRight = 8;   // AIN1/PH
constexpr uint8_t kPinDirLeft = 7;    // BIN1/PH
constexpr uint8_t kPinStandby = 0xFF;
constexpr bool kHasStandby = false;
#else
#error "select a shield: -DSHIELD_SMARTCAR_V11_TB6612 or -DSHIELD_TANK_DRV8835"
#endif

// Direction pin level that drives each track forward. Measured by the shield probe on the owner's
// tank (2026-10-13): both forward = HIGH (the ELEGOO starting guess had right = LOW; it ran backward).
constexpr bool kDirRightForwardHigh = true;
constexpr bool kDirLeftForwardHigh = true;

constexpr uint8_t kPinButton = 2;
constexpr uint8_t kPinLed = 4;        // Uno PD4, Mega PG5 (the LED driver picks the port)
constexpr uint8_t kPinServoPan = 10;
constexpr uint8_t kPinServoTilt = 11;
constexpr uint8_t kPinEcho = 12;      // Uno PB4 (PCINT4), Mega PB6 (PCINT6); both vector PCINT0
constexpr uint8_t kPinTrig = 13;

#if defined(BOARD_MEGA)
// LLD §11.4 (no hardware rework): the shield plugs on as-is. IMU on A4/A5 via software I2C,
// IR on D9 via the 20 kHz Timer1 sampler; encoder cables plug directly into A8/A9 (L), A10/A11 (R).
constexpr uint8_t kPinIr = 9;           // shield D9 = PH6 (no PCINT on the Mega): Timer1-sampled
constexpr uint8_t kPinBuzzer = 54;      // A0 (shield buzzer port, as on the Uno); tone() on Timer2
constexpr uint8_t kPinEncLeft = 62;     // A8 = PK0 (PCINT16, vector PCINT2), left channel A
constexpr uint8_t kPinEncRight = 64;    // A10 = PK2 (PCINT18, vector PCINT2), right channel A
constexpr uint8_t kPinEncLeftB = 63;    // A9 = PK1, left channel B (wired for quadrature later; unused)
constexpr uint8_t kPinEncRightB = 65;   // A11 = PK3, right channel B (wired for quadrature later; unused)
constexpr uint8_t kPinBattery = 57;     // A3 (ADC3)
#else
constexpr uint8_t kPinIr = 9;           // PB1, PCINT1 (vector PCINT0)
constexpr uint8_t kPinBuzzer = 14;      // A0
constexpr uint8_t kPinEncLeft = 15;     // A1 = PC1 (PCINT9, vector PCINT1)
constexpr uint8_t kPinEncRight = 16;    // A2 = PC2 (PCINT10, vector PCINT1)
constexpr uint8_t kPinBattery = 17;     // A3
#endif

constexpr uint8_t kImuAddress = 0x68;
// Tank axis <- sensor axis (1 = X, 2 = Y, 3 = Z; negative = inverted). Measured by the mount check.
constexpr int8_t kImuMapX = 1;
constexpr int8_t kImuMapY = 2;
constexpr int8_t kImuMapZ = 3;

constexpr uint32_t kBaud = 250000UL;
constexpr uint16_t kFwVersion = 0x0100;  // 1.0

// Set by scripts/git_commit.py (uno env); 0 = built outside git / unknown. Sent in HELLO.
#ifndef CQ_GIT_COMMIT
#define CQ_GIT_COMMIT 0UL
#endif
constexpr uint32_t kGitCommit = CQ_GIT_COMMIT;

}  // namespace board
}  // namespace cq
