// ELEGOO ESP32S3-Camera-V1.0: pins, ports and sizes (bridge LLD R2.1, design-notes §24).
#pragma once

#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace board {

constexpr const char* kBoardName = "ESP32S3-Camera-V1.0";
constexpr const char* kExpectedMac = "48:ca:43:9e:74:b4";  // owner's board (warning only)

// Uno link: Serial1. GPIO3 is a strapping pin: input only after boot, no pull-down.
constexpr int kUnoRxPin = 3;    // <- Uno TX
constexpr int kUnoTxPin = 40;   // -> Uno RX
constexpr uint32_t kUnoBaud = 250000UL;
static_assert(kUnoBaud == proto::kBaud, "UART baud must match protocol/defs/meta.yaml");
constexpr int kUartRxBufSize = 2048;
constexpr int kUartTxBufSize = 1024;

constexpr uint32_t kLogBaud = 115200UL;  // USB-CDC log
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kStreamPort = 81;
constexpr uint16_t kOtaPort = 3232;

constexpr const char* kHostname = "conqueror-bridge";  // mDNS conqueror-bridge.local
constexpr const char* kFallbackSsid = "conqueror-bridge";
constexpr uint32_t kStatusPeriodMs = 10000;
constexpr uint32_t kLinkTickMs = 2;

constexpr uint8_t kMaxStreamViewers = 2;

// Camera: ESP32-S3-EYE map (design-notes §24). -1 = not connected.
constexpr int kCamPwdn = -1;
constexpr int kCamReset = -1;
constexpr int kCamXclk = 15;
constexpr int kCamSda = 4;
constexpr int kCamScl = 5;
constexpr int kCamD7 = 16;
constexpr int kCamD6 = 17;
constexpr int kCamD5 = 18;
constexpr int kCamD4 = 12;
constexpr int kCamD3 = 10;
constexpr int kCamD2 = 8;
constexpr int kCamD1 = 9;
constexpr int kCamD0 = 11;
constexpr int kCamVsync = 6;
constexpr int kCamHref = 7;
constexpr int kCamPclk = 13;
// Owner 2026-10-18: camera back on - its clock pin at the weakest drive no longer disturbs the WiFi (LLD R12).
// false = never started: /stream and /capture answer 503, the page shows no video.
constexpr bool kCameraEnabled = true;
// 20 MHz (design-notes §143, 2026-10-24): with the weakest XCLK drive (below) the WiFi stays clean - 0 of 600 pings
// lost while streaming - and the camera captures 27 QVGA / 24 VGA pictures a second (14 / 7 at 10 MHz). At 20 MHz with
// the default drive the WiFi had lost half its packets (LLD R4.14, 2026-10-16); the drive was the cause (§76).
constexpr uint32_t kCamXclkHz = 20000000UL;
// Gentle XCLK edges: at the driver's default drive 2 the clock reaches the antenna (83-100 % ping loss); drive 0 = 0 %
// (LLD R12, design-notes §76).
constexpr int kCamXclkDrive = 0;  // GPIO_DRIVE_CAP_0 (weakest of 0..3)

}  // namespace board
}  // namespace cq
