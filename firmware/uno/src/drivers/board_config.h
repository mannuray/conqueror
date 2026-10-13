#pragma once
#include <stdint.h>

#if !defined(BOARD_UNO)
#error "select a board: -DBOARD_UNO"
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

// Direction pin level that drives each track forward. Measured by the shield probe (Task 7);
// starting values from the ELEGOO tank firmware (right forward = LOW, left forward = HIGH).
constexpr bool kDirRightForwardHigh = false;
constexpr bool kDirLeftForwardHigh = true;

constexpr uint8_t kPinButton = 2;
constexpr uint8_t kPinLed = 4;        // PD4
constexpr uint8_t kPinIr = 9;         // PB1, PCINT1 (vector PCINT0)
constexpr uint8_t kPinServoPan = 10;
constexpr uint8_t kPinServoTilt = 11;
constexpr uint8_t kPinEcho = 12;      // PB4, PCINT4 (vector PCINT0)
constexpr uint8_t kPinTrig = 13;
constexpr uint8_t kPinBuzzer = 14;    // A0
constexpr uint8_t kPinEncLeft = 15;   // A1 = PC1 (vector PCINT1)
constexpr uint8_t kPinEncRight = 16;  // A2 = PC2 (vector PCINT1)
constexpr uint8_t kPinBattery = 17;   // A3

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
