// Bridge motion service tuning (bridge LLD R4.3 / R4.4). Constants, not settings: tuned on the floor (Plan H Task 7).
#pragma once

#include <stdint.h>

namespace cq {
namespace core {
namespace motion {

// TURN (spin): angles in 0.01°, rates in 0.1 °/s, + = CCW / left (the Uno's yaw convention).
constexpr int32_t kTurnKpPctPerDeg = 1;      // w = 1 % per degree still to go
constexpr int8_t kTurnWMin = 12;             // % - smallest command that still turns
constexpr int32_t kLatencyDdpsToCdeg = 8;    // e_p = e - rate * 80 ms  ->  rate_ddps * 8 / 10 (0.01°)
constexpr int32_t kWindowInCdeg = 150;       // done window: |e| <= 1.5° ...
constexpr int32_t kWindowOutCdeg = 250;      // ... left again only beyond 2.5° (no hunting around the target)
constexpr int16_t kWindowRateDdps = 50;      // ... and |rate| < 5 °/s
constexpr uint32_t kWindowHoldMs = 200;      // ... for 200 ms
constexpr int16_t kStallRateDdps = 20;       // stalled: |rate| < 2 °/s while commanding >= w_min ...
constexpr uint32_t kStallMs = 1500;          // ... for 1.5 s
constexpr uint32_t kTimeoutBaseMs = 3000;    // timeout = |angle| / 20 °/s + 3 s

// DRIVE_RATE: w = ff * r + Kp * err + Ki * integral(err), |w| <= 100, integration stops while saturated.
constexpr float kRateFfPctPerDps = 0.45f;  // floor data 2026-10-15: w 60 % spin ~ 110-137 °/s
constexpr float kRateKpPctPerDps = 0.3f;
constexpr float kRateKiPctPerDeg = 0.6f;   // per (°/s · s)
constexpr uint32_t kRateDeadmanMs = 300;   // no drive_rate update for 300 ms -> stop (as drive)
constexpr int8_t kRateSaturatedPct = 95;   // C1 guard: |w| >= 95 % ...
constexpr int16_t kRateWrongWayDdps = 50;  // ... while turning > 5 °/s the other way ...
constexpr uint32_t kRateWrongWayMs = 500;  // ... for 0.5 s -> end (diverged)

// Service
constexpr uint32_t kTelemGapMs = 200;      // no TELEM_FAST for 200 ms while active -> end (link)

}  // namespace motion
}  // namespace core
}  // namespace cq
