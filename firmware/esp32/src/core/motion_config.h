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
// Stopped (R18; the floor test 2026-10-16, LLD R4.16, taught to judge it by the heading - a shaking tank reads
// +-55 °/s on target): the heading moved < 2° over 300 ms (avg < 6.7 °/s), else the hold restarts.
constexpr uint32_t kWindowHoldMs = 300;
constexpr int32_t kSettleMoveCdeg = 200;
constexpr int16_t kStallRateDdps = 20;       // stalled: |rate| < 2 °/s while commanding >= w_min ...
constexpr uint32_t kStallMs = 1500;          // ... for 1.5 s
constexpr uint32_t kTimeoutBaseMs = 3000;    // timeout = |angle| / 20 °/s + 3 s
// R18 (design-notes §112): approach, slow to the slowest smooth spin, cut at the predicted stop, at most one nudge.
constexpr int32_t kTurnSlowZoneCdeg = 2500;  // the last 25° under rate control at kSpinMinDdps
constexpr int32_t kTurnDoneCdeg = 300;       // stopped within 3°: done; further off: one nudge, then done anyway
constexpr uint16_t kCoastDefaultMs = 150;    // T_coast: predicted stop = pos + rate x T_coast (learned, NVS); floor
                                             // 2026-10-21: ~150 ms (the Uno's ramp-down + the link + the glide, §113)
constexpr uint16_t kCutLeadMs = 10;          // R18.7: cut half a 20 ms frame early - the stop centres on the target
constexpr uint16_t kCoastMinMs = 20;         // a reading counts only within 20..400 ms ...
constexpr uint16_t kCoastMaxMs = 400;
constexpr int16_t kCoastMinRateDdps = 300;   // ... and with a cut rate of at least 30 °/s

// DRIVE_RATE: w = ff * r + Kp * err + Ki * integral(err), |w| <= 100, integration stops while saturated.
constexpr float kRateFfPctPerDps = 0.45f;  // floor data 2026-10-15: w 60 % spin ~ 110-137 °/s
constexpr float kRateKpPctPerDps = 0.3f;
constexpr float kRateKiPctPerDeg = 0.6f;   // per (°/s · s)
constexpr uint32_t kRateDeadmanMs = 300;   // no drive_rate update for 300 ms -> stop (as drive)
constexpr int8_t kRateSaturatedPct = 95;   // C1 guard: |w| >= 95 % ...
constexpr int16_t kRateWrongWayDdps = 50;  // ... while turning > 5 °/s the other way ...
constexpr uint32_t kRateWrongWayMs = 500;  // ... for 0.5 s -> end (diverged)

// MOVE by distance (Plan K, bridge LLD R7.3): counts from TELEM_FAST enc_l / enc_r (signed by drive direction).
constexpr int32_t kMoveSlowCm = 10;            // slow-down zone before the target
constexpr int32_t kMoveCrawlPct = 15;          // slowest v that still moves both tracks (floor-tuned)
constexpr uint8_t kMoveSpeedMin = 10;          // move speed 10..100 %
constexpr int32_t kMoveMaxCm = 500;            // |dist_cm| 1..500
constexpr uint32_t kMoveStallMs = 1500;        // stalled: < kMoveStallCm progress for 1.5 s
constexpr int32_t kMoveStallCm = 1;
constexpr uint32_t kMoveBlockedMs = 2000;      // blocked flag (obstacle brake) for 2 s -> end blocked
constexpr uint16_t kMoveObstacleMm = 250;      // no progress with an obstacle this close ahead -> blocked (the Uno's
                                               // slow-down stops the tank without its blocked flag, floor 2026-10-17)
constexpr int32_t kMoveTimeoutCmPerS = 5;      // timeout = |cm| / 5 cm/s + 3 s
constexpr uint32_t kMoveTimeoutBaseMs = 3000;
// counts_per_m (Plan K, spec 02 §G4): NVS, default from the 160 cm tape run (design-notes §63).
constexpr uint16_t kCpmDefault = 7560;
constexpr uint16_t kCpmMin = 3000;
constexpr uint16_t kCpmMax = 15000;
inline bool validCpm(uint32_t n) { return n >= kCpmMin && n <= kCpmMax; }
// Learned early stop (spec 02 §G6): roll-on after the STOP, mm.
constexpr uint16_t kRollDefaultMm = 25;     // floor 2026-10-17: 24-28 mm at the crawl
constexpr int32_t kRollMaxMm = 80;          // readings above are discarded
constexpr uint32_t kRollSettleMs = 100;     // counts unchanged this long -> settled
constexpr uint32_t kRollWindowMs = 600;     // ... or at most this long after the STOP

// Arcs and pivots (Plan L, spec 02 §H1): floor-measured model 2026-10-17, re-measurable.
constexpr double kTrackWidthCm = 19.0;          // B, centre to centre (pivot radius 9.5 cm)
constexpr double kTrackSpeedOffsetCmS = 15.5;   // track speed v(p) = 15.5 + 0.18 p cm/s
constexpr double kTrackSpeedSlopeCmS = 0.18;
constexpr double kTrackVMinCmS = 17.7;          // a track runs at 0 or >= this (the Uno's dead-band lift)
constexpr double kTrackVMaxCmS = 33.5;          // full power
constexpr int16_t kSpinMinDdps = 650;           // slowest smooth spin: 65 °/s
constexpr float kArcFfPctPerDps = 1.15f;        // arc feed-forward: 1 % of w gives ~0.85 °/s with both tracks forward
                                                // (floor 2026-10-18, R 60 @ 40 %); spins keep kRateFfPctPerDps 0.45
constexpr uint32_t kArcSpeedWindowMs = 100;     // measured speed over the newest frames >= 100 ms apart
constexpr uint32_t kArcStallMs = 1500;          // < 1 cm of counts and < 2° of heading for 1.5 s -> stalled
constexpr int32_t kArcStallCdeg = 200;
constexpr int32_t kArcTimeoutCmPerS = 5;        // timeout = arc length / 5 cm/s + 3 s
constexpr uint32_t kArcTimeoutBaseMs = 3000;

// Service
constexpr uint32_t kTelemGapMs = 200;      // no TELEM_FAST for 200 ms while active -> end (link)

}  // namespace motion
}  // namespace core
}  // namespace cq
