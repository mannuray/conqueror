# Plan L — Arcs, pivots, minimum drive_rate (bridge) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ESP32 bridge runs gyro-closed arcs (`turn` + `radius_cm`), pivots (`turn` + `pivot`), refuses arcs outside their speed band with a helpful message, and clamps spinning `drive_rate` to the slowest smooth 65 °/s.

**Architecture:** A new `ArcController` (measured speed → ω = v / R → the existing `RateLoop`) and band functions (`arc_limits`) join the bridge's `MotionService`; the pivot reuses `TurnController` with v = |w| so the inside track gets 0 under the Uno's plain AUTO mix. The link task routes and refuses; the Uno only ever sees DRIVE frames.

**Tech Stack:** C++17 ESP32-S3 Arduino (PlatformIO `bridge` / `bridge_ota`), Unity host tests (`pio test -e native`), Python 3.12 + uv (ruff, pyright strict, pytest).

**Spec:** `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part H; LLD `docs/lld/esp32-bridge-lld.md` Revision 8.

## Global Constraints

- Bridge only: **no Uno change, no `protocol/defs` change** (protocol 3).
- Conventions: + = counter-clockwise / left; radius to the tank's centre; arcs forward only.
- Model (spec H1): B = 19 cm; v(p) = 15.5 + 0.18·p cm/s; v_min 17.7 cm/s; v_max 33.5 cm/s; slowest smooth spin 65 °/s; counts_per_m from NVS (7514).
- Band (spec H3): lo = ⌈p(v_min/(1 − k))⌉, hi = ⌊p(v_max/(1 + k))⌋, k = B/2R, lo ≥ 12; table R 31/40/49/60/80/100/150 → 56 / 43–64 / 36–69 / 31–74 / 26–80 / 23–83 / 19–88; R < 31 impossible.
- Refusal messages exactly: `radius 40 cm: use speed 43-64 %, or pivot` and `radius 20 cm too tight (min 31): use pivot` (INVALID_COMMAND).
- Spin, `drive_rate` with v ≠ 0, and `move` behaviour unchanged: all existing host tests stay green.
- Safety never weakened: only DRIVE frames src AUTO; every end STOPs while the client owns the tank.
- Whole numbers in the controllers except the band maths (double); `-Wall -Wextra -Werror`.
- Commits: Conventional Commits ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch **`plan-l`**; ask before merging.
- Hardware: owner present; **ask "stand or floor?" and say what moves before every drive** (memory rule).

## Review Focus

1. **Uno restart mid-arc** (counts drop to 0): the measured speed must never go negative and turn the tank the other way — test in Task 2 (`test_arc_counts_reset_never_turns_backwards`).
2. **Arc of more than one turn** (`by` 400): the unwrapped heading must carry through 360° and end at 400 — test in Task 2 (`test_arc_more_than_one_turn`).
3. **Default speed outside the radius's band** (R 31 at the default 40 %): refused with the band, never driven — test in Task 1 (`test_arc_refusal_messages` covers the R 31 text) and Task 6 glue.
4. **Pivot asked slower than the minimum** (speed 12): the outside track still gets 24 % (w_min 12), never a stalled 12 % — test in Task 3 (`test_service_pivot_speed_floor`).
5. **drive_rate at tiny negative rates** (−1 °/s with v = 0): clamped to −65 °/s, not to +65 and not to 0 — test in Task 4 (`test_rate_clamp_to_minimum_spin`).

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/esp32/src/core/motion_config.h` (modify) | H1 model constants, arc stall / timeout constants |
| `firmware/esp32/src/core/arc_limits.h/.cpp` (create) | speed band, minimum radius, refusal text |
| `firmware/esp32/src/core/arc_controller.h/.cpp` (create) | `ArcStep`, `ArcController` |
| `firmware/esp32/src/core/motion_service.h/.cpp` (modify) | `kArc` / `kPivot`, `startArc`, `startPivot`, drive_rate clamp, `limited()` |
| `firmware/esp32/src/core/motion_link.cpp` (modify) | Uno stall / encoder-fault end arcs too |
| `firmware/esp32/src/core/motion_router.h/.cpp` (modify) | `routeTurn(caps, radius, pivot)` |
| `firmware/esp32/src/core/ws_codec.h/.cpp` (modify) | `pivot` field |
| `firmware/esp32/src/core/ws_json.h/.cpp` (modify) | `PrimitiveView.limited`, `jsonNackMessage` |
| `firmware/esp32/src/net/link_task.cpp` (modify) | `handleTurn` arcs / pivots / refusal, `limited` in tf |
| `firmware/esp32/test/test_arc/test_main.cpp` (create) | band + ArcController tests |
| `firmware/esp32/test/test_motion`, `test_ws_codec`, `test_ws_json` (modify) | service, link, router, codec, JSON tests |
| `tools/smoke/turn.py`, `tools/smoke/tests/test_turn_math.py` (create) | the tool and its maths test |

Branch first: `git checkout main && git pull --ff-only && git checkout -b plan-l`.

---

### Task 1: Speed band, minimum radius, refusal text

**Files:**
- Modify: `firmware/esp32/src/core/motion_config.h`
- Create: `firmware/esp32/src/core/arc_limits.h`, `firmware/esp32/src/core/arc_limits.cpp`
- Test: `firmware/esp32/test/test_arc/test_main.cpp` (new suite)

**Interfaces:**
- Produces: `struct ArcBand { bool possible; uint8_t lo; uint8_t hi; }`, `ArcBand arcBand(uint16_t radius_cm)`, `uint16_t arcMinRadiusCm()`, `size_t arcRefusal(char* out, size_t cap, uint16_t radius_cm)`; constants `motion::kTrackWidthCm`, `kTrackSpeedOffsetCmS`, `kTrackSpeedSlopeCmS`, `kTrackVMinCmS`, `kTrackVMaxCmS`, `kSpinMinDdps`, `kArcSpeedWindowMs`, `kArcStallMs`, `kArcStallCdeg`, `kArcTimeoutCmPerS`, `kArcTimeoutBaseMs`.

- [ ] **Step 1: Constants** — append inside `namespace motion` in `motion_config.h`, before `// Service`:

```cpp
// Arcs and pivots (Plan L, spec 02 §H1): floor-measured model 2026-10-17, re-measurable.
constexpr double kTrackWidthCm = 19.0;          // B, centre to centre (pivot radius 9.5 cm)
constexpr double kTrackSpeedOffsetCmS = 15.5;   // track speed v(p) = 15.5 + 0.18 p cm/s
constexpr double kTrackSpeedSlopeCmS = 0.18;
constexpr double kTrackVMinCmS = 17.7;          // a track runs at 0 or >= this (the Uno's dead-band lift)
constexpr double kTrackVMaxCmS = 33.5;          // full power
constexpr int16_t kSpinMinDdps = 650;           // slowest smooth spin: 65 °/s
constexpr uint32_t kArcSpeedWindowMs = 100;     // measured speed over the newest frames >= 100 ms apart
constexpr uint32_t kArcStallMs = 1500;          // < 1 cm of counts and < 2° of heading for 1.5 s -> stalled
constexpr int32_t kArcStallCdeg = 200;
constexpr int32_t kArcTimeoutCmPerS = 5;        // timeout = arc length / 5 cm/s + 3 s
constexpr uint32_t kArcTimeoutBaseMs = 3000;
```

- [ ] **Step 2: Failing tests** — create `firmware/esp32/test/test_arc/test_main.cpp`:

```cpp
// Plan L (bridge LLD R8): arc speed band, minimum radius, refusal text; ArcController (Task 2).
#include <math.h>
#include <string.h>
#include <unity.h>

#include "core/arc_limits.h"
#include "core/motion_config.h"
#include "cq_protocol.h"

using namespace cq::core;
namespace proto = cq::proto;

void setUp() {}
void tearDown() {}

void test_arc_band_matches_spec_table() {
  const struct { uint16_t r; uint8_t lo, hi; } cases[] = {{31, 56, 56},  {40, 43, 64},  {49, 36, 69}, {60, 31, 74},
                                                           {80, 26, 80},  {100, 23, 83}, {150, 19, 88}};
  for (const auto& c : cases) {
    const ArcBand b = arcBand(c.r);
    TEST_ASSERT_TRUE(b.possible);
    TEST_ASSERT_EQUAL_UINT8(c.lo, b.lo);
    TEST_ASSERT_EQUAL_UINT8(c.hi, b.hi);
  }
}

void test_arc_too_tight_and_min_radius() {
  TEST_ASSERT_FALSE(arcBand(30).possible);  // inner needs > outer allows
  TEST_ASSERT_FALSE(arcBand(9).possible);   // inside B/2: the inner track would reverse
  TEST_ASSERT_FALSE(arcBand(0).possible);
  TEST_ASSERT_EQUAL_UINT16(31, arcMinRadiusCm());
  const ArcBand wide = arcBand(1000);
  TEST_ASSERT_TRUE(wide.possible);
  TEST_ASSERT_TRUE(wide.lo >= motion::kTurnWMin && wide.hi <= 100);
}

void test_arc_refusal_messages() {
  char buf[96];
  TEST_ASSERT_TRUE(arcRefusal(buf, sizeof(buf), 40) > 0);
  TEST_ASSERT_EQUAL_STRING("radius 40 cm: use speed 43-64 %, or pivot", buf);
  arcRefusal(buf, sizeof(buf), 31);
  TEST_ASSERT_EQUAL_STRING("radius 31 cm: use speed 56-56 %, or pivot", buf);
  arcRefusal(buf, sizeof(buf), 20);
  TEST_ASSERT_EQUAL_STRING("radius 20 cm too tight (min 31): use pivot", buf);
  TEST_ASSERT_EQUAL(0, arcRefusal(buf, 8, 40));  // does not fit -> 0
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_arc_band_matches_spec_table);
  RUN_TEST(test_arc_too_tight_and_min_radius);
  RUN_TEST(test_arc_refusal_messages);
  return UNITY_END();
}
```

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_arc`
Expected: build FAIL — `core/arc_limits.h` not found.

- [ ] **Step 3: Implement** — `firmware/esp32/src/core/arc_limits.h`:

```cpp
// Arc radius / speed band (Plan L, spec 02 §H3, bridge LLD R8.3): an arc of radius R at centre speed v needs the
// inner track at v·(1 − B/2R) >= v_min and the outer at v·(1 + B/2R) <= v_max, so each radius has a usable speed
// band (model constants: motion_config.h).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

struct ArcBand {
  bool possible = false;
  uint8_t lo = 0;  // speed %
  uint8_t hi = 0;
};

ArcBand arcBand(uint16_t radius_cm);
uint16_t arcMinRadiusCm();  // smallest radius with a usable band (31 with the H1 model)
// "radius 40 cm: use speed 43-64 %, or pivot" / "radius 20 cm too tight (min 31): use pivot"; 0 when it does not fit.
size_t arcRefusal(char* out, size_t cap, uint16_t radius_cm);

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/arc_limits.cpp`:

```cpp
#include "core/arc_limits.h"

#include <math.h>
#include <stdio.h>

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
double pctFor(double v_cm_s) { return (v_cm_s - motion::kTrackSpeedOffsetCmS) / motion::kTrackSpeedSlopeCmS; }
}  // namespace

ArcBand arcBand(uint16_t radius_cm) {
  ArcBand b;
  if (radius_cm == 0) return b;
  const double k = motion::kTrackWidthCm / 2.0 / radius_cm;
  if (k >= 1.0) return b;  // inside B/2: the inner track would have to reverse
  double lo = ceil(pctFor(motion::kTrackVMinCmS / (1.0 - k)));
  double hi = floor(pctFor(motion::kTrackVMaxCmS / (1.0 + k)));
  if (lo < motion::kTurnWMin) lo = motion::kTurnWMin;
  if (hi > 100.0) hi = 100.0;
  if (lo > hi) return b;
  b.possible = true;
  b.lo = static_cast<uint8_t>(lo);
  b.hi = static_cast<uint8_t>(hi);
  return b;
}

uint16_t arcMinRadiusCm() {
  for (uint16_t r = 1; r < 1000; ++r) {
    if (arcBand(r).possible) return r;
  }
  return 1000;
}

size_t arcRefusal(char* out, size_t cap, uint16_t radius_cm) {
  const ArcBand b = arcBand(radius_cm);
  const int n = b.possible ? snprintf(out, cap, "radius %u cm: use speed %u-%u %%, or pivot",
                                      static_cast<unsigned>(radius_cm), static_cast<unsigned>(b.lo),
                                      static_cast<unsigned>(b.hi))
                           : snprintf(out, cap, "radius %u cm too tight (min %u): use pivot",
                                      static_cast<unsigned>(radius_cm), static_cast<unsigned>(arcMinRadiusCm()));
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 4: Run** — `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native` → all PASS (3 new). If a band cell is off by one, print the double value (`pctFor` result) before touching the rounding: the spec table was generated with exactly this ceil / floor in double precision.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/motion_config.h firmware/esp32/src/core/arc_limits.* firmware/esp32/test/test_arc/test_main.cpp
git commit -m "feat(bridge): arc speed band, minimum radius and refusal text (plan L)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `ArcController`

**Files:**
- Create: `firmware/esp32/src/core/arc_controller.h`, `firmware/esp32/src/core/arc_controller.cpp`
- Test: `firmware/esp32/test/test_arc/test_main.cpp`

**Interfaces:**
- Consumes: `MotionTelem` (`core/move_controller.h`), `RateLoop` (`core/rate_loop.h`: `reset()`, `int8_t step(int16_t target_ddps, int16_t rate_ddps, uint16_t dt_ms)`), `wrapDeltaCdeg` (`core/turn_controller.h`), Task 1 constants, `motion::kLatencyDdpsToCdeg`, `kMoveBlockedMs`, `kMoveObstacleMm`, `kRateSaturatedPct`, `kRateWrongWayDdps`, `kRateWrongWayMs`.
- Produces: `struct ArcStep { int8_t v; int8_t w; bool done, stalled, blocked, timeout, diverged; }`; `class ArcController { void begin(const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm, uint32_t now_ms); ArcStep step(const MotionTelem& t, uint32_t now_ms); uint8_t progressPct() const; int16_t targetDdps() const; }`.

- [ ] **Step 1: Failing tests** — add `#include "core/arc_controller.h"` to `test/test_arc/test_main.cpp`, and before `int main()`:

```cpp
// ---------- ArcController against a plant ----------
namespace {
constexpr uint16_t kCpm = 7560;
// Counts follow v (spec H1 speed model, 75.6 counts / cm) and the turn rate follows w (2.2 °/s per %, 150 ms lag),
// both acting on the command sent 3 frames earlier (60 ms).
struct ArcPlant {
  double yaw_deg = 0.0, rate_dps = 0.0, counts = 0.0;
  int8_t hv[3] = {0, 0, 0}, hw[3] = {0, 0, 0};
  uint8_t head = 0;
  uint32_t flags = 0;
  uint16_t dist_mm = 0xFFFF;
  void step(int8_t v, int8_t w) {
    const int8_t av = hv[head], aw = hw[head];
    hv[head] = v;
    hw[head] = w;
    head = static_cast<uint8_t>((head + 1) % 3);
    const double v_cm = av > 0 ? motion::kTrackSpeedOffsetCmS + motion::kTrackSpeedSlopeCmS * av : 0.0;
    counts += v_cm * 75.6 * 0.02;
    rate_dps += (aw * 2.2 - rate_dps) * (0.02 / 0.15);
    yaw_deg += rate_dps * 0.02;
  }
  MotionTelem telem() const {
    MotionTelem t;
    double y = fmod(yaw_deg, 360.0);
    if (y < 0) y += 360.0;
    t.yaw_cdeg = static_cast<uint16_t>(lround(y * 100.0) % 36000);
    t.rate_ddps = static_cast<int16_t>(lround(rate_dps * 10.0));
    t.enc_l = t.enc_r = static_cast<int32_t>(counts);
    t.flags = flags;
    t.dist_mm = dist_mm;
    return t;
  }
};

ArcStep runArc(ArcController& ac, ArcPlant& p, uint32_t& t, uint32_t limit_ms) {
  ArcStep s;
  while (t < limit_ms) {
    t += 20;
    s = ac.step(p.telem(), t);
    if (s.done || s.stalled || s.blocked || s.timeout || s.diverged) break;
    p.step(s.v, s.w);
  }
  return s;
}
}  // namespace

void test_arc_left_and_right_end_at_heading() {
  for (int dir = -1; dir <= 1; dir += 2) {
    ArcPlant p;
    ArcController ac;
    uint32_t t = 0;
    ac.begin(p.telem(), dir * 9000, 60, 40, kCpm, 0);
    const ArcStep s = runArc(ac, p, t, 40000);
    TEST_ASSERT_TRUE(s.done);
    TEST_ASSERT_DOUBLE_WITHIN(2.0, dir * 90.0, p.yaw_deg);
    TEST_ASSERT_EQUAL_UINT8(100, ac.progressPct());
  }
}

void test_arc_rate_tracks_v_over_r() {
  // 3 s in: v = 22.7 cm/s -> ω = 57.3 · 22.7 / 60 = 21.7 °/s; the gyro rate is within 15 % of the target
  ArcPlant q;
  ArcController bc;
  uint32_t u = 0;
  bc.begin(q.telem(), 18000, 60, 40, kCpm, 0);
  for (int i = 0; i < 150; ++i) {
    const ArcStep s = bc.step(q.telem(), u += 20);
    q.step(s.v, s.w);
  }
  TEST_ASSERT_INT_WITHIN(25, 217, bc.targetDdps());
  TEST_ASSERT_DOUBLE_WITHIN(0.15 * 21.7, 21.7, q.rate_dps);
  TEST_ASSERT_TRUE(bc.progressPct() > 0 && bc.progressPct() < 100);
}

void test_arc_stalled_and_blocked() {
  {
    ArcPlant p;  // never stepped: no counts, no heading
    ArcController ac;
    ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
    ArcStep s;
    for (uint32_t t = 20; t < motion::kArcStallMs; t += 20) TEST_ASSERT_FALSE(ac.step(p.telem(), t).stalled);
    s = ac.step(p.telem(), motion::kArcStallMs);
    TEST_ASSERT_TRUE(s.stalled);
  }
  {
    ArcPlant p;
    p.dist_mm = 150;  // stopped with a box close ahead -> blocked
    ArcController ac;
    ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
    TEST_ASSERT_TRUE(ac.step(p.telem(), motion::kArcStallMs).blocked);
  }
  {
    ArcPlant p;
    p.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);
    ArcController ac;
    ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
    ArcStep s;
    for (uint32_t t = 20; t < motion::kMoveBlockedMs; t += 20) {
      s = ac.step(p.telem(), t);
      TEST_ASSERT_FALSE(s.blocked || s.stalled);
    }
    TEST_ASSERT_TRUE(ac.step(p.telem(), motion::kMoveBlockedMs + 20).blocked);
  }
}

void test_arc_timeout() {
  ArcController ac;
  MotionTelem tel;
  ac.begin(tel, 9000, 60, 40, kCpm, 0);  // 94 cm of arc -> 18.8 s + 3 s
  const uint32_t deadline = 94u * 1000u / motion::kArcTimeoutCmPerS + motion::kArcTimeoutBaseMs;
  for (uint32_t t = 20; t < deadline; t += 20) {
    if (t % 1400 == 0) tel.enc_l += 80, tel.enc_r += 80;  // creeping: never stalled, never turning
    TEST_ASSERT_FALSE(ac.step(tel, t).timeout);
  }
  TEST_ASSERT_TRUE(ac.step(tel, deadline).timeout);
}

void test_arc_diverged() {
  ArcController ac;
  MotionTelem tel;
  ac.begin(tel, 9000, 60, 40, kCpm, 0);
  ArcStep s;
  double yaw = 0.0;
  uint32_t t = 0;
  for (int i = 0; i < 400 && !s.diverged; ++i) {
    tel.enc_l += 30;  // moving
    tel.enc_r += 30;
    yaw -= 0.6;  // ... but turning right at 30 °/s while asked left
    double y = fmod(yaw, 360.0);
    if (y < 0) y += 360.0;
    tel.yaw_cdeg = static_cast<uint16_t>(lround(y * 100.0) % 36000);
    tel.rate_ddps = -300;
    s = ac.step(tel, t += 20);
    TEST_ASSERT_FALSE(s.done || s.stalled || s.timeout);
  }
  TEST_ASSERT_TRUE(s.diverged);
  TEST_ASSERT_EQUAL_INT8(0, s.w);
}

void test_arc_counts_reset_never_turns_backwards() {  // Review Focus 1
  ArcPlant p;
  ArcController ac;
  uint32_t t = 0;
  ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
  for (int i = 0; i < 50; ++i) {
    const ArcStep s = ac.step(p.telem(), t += 20);
    p.step(s.v, s.w);
  }
  p.counts = 0.0;  // Uno restarted
  for (int i = 0; i < 10; ++i) {
    const ArcStep s = ac.step(p.telem(), t += 20);
    TEST_ASSERT_TRUE(ac.targetDdps() >= 0);  // a left arc never asks for a right turn
    if (s.done || s.stalled || s.blocked) break;
    p.step(s.v, s.w);
  }
}

void test_arc_more_than_one_turn() {  // Review Focus 2
  ArcPlant p;
  ArcController ac;
  uint32_t t = 0;
  ac.begin(p.telem(), 40000, 100, 40, kCpm, 0);  // 400° on a 1 m radius
  const ArcStep s = runArc(ac, p, t, 120000);
  TEST_ASSERT_TRUE(s.done);
  TEST_ASSERT_DOUBLE_WITHIN(3.0, 400.0, p.yaw_deg);
}
```

and the RUN_TESTs:

```cpp
  RUN_TEST(test_arc_left_and_right_end_at_heading);
  RUN_TEST(test_arc_rate_tracks_v_over_r);
  RUN_TEST(test_arc_stalled_and_blocked);
  RUN_TEST(test_arc_timeout);
  RUN_TEST(test_arc_diverged);
  RUN_TEST(test_arc_counts_reset_never_turns_backwards);
  RUN_TEST(test_arc_more_than_one_turn);
```

Run: `pio test -e native -f test_arc` → build FAIL (`core/arc_controller.h` not found).

- [ ] **Step 2: Implement** — `firmware/esp32/src/core/arc_controller.h`:

```cpp
// Arc turn on the bridge (Plan L, spec 02 §H4, bridge LLD R8.3): forward at `speed`; the turn rate is gyro-closed on
// ω = v_meas / R with v_meas from the encoders, through the drive_rate loop; ends at the target heading.
#pragma once

#include <stdint.h>

#include "core/move_controller.h"
#include "core/rate_loop.h"

namespace cq {
namespace core {

struct ArcStep {
  int8_t v = 0;
  int8_t w = 0;
  bool done = false;
  bool stalled = false;
  bool blocked = false;
  bool timeout = false;
  bool diverged = false;
};

class ArcController {
 public:
  void begin(const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm,
             uint32_t now_ms);
  ArcStep step(const MotionTelem& t, uint32_t now_ms);
  uint8_t progressPct() const;
  int16_t targetDdps() const { return m_target_ddps; }  // last ω asked of the rate loop (0.1 °/s)

 private:
  int32_t speedCmS10(int32_t counts, uint32_t now_ms);  // measured centre speed, 0.1 cm/s, never negative

  RateLoop m_rate;
  int32_t m_delta = 0;  // target, 0.01°
  int32_t m_pos = 0;    // turned, unwrapped, 0.01°
  int8_t m_dir = 1;
  uint16_t m_last_yaw = 0;
  uint16_t m_radius = 1;
  uint16_t m_cpm = 7560;
  uint8_t m_speed = 40;
  int16_t m_target_ddps = 0;
  uint32_t m_last_ms = 0;
  uint32_t m_deadline = 0;
  static constexpr uint8_t kHist = 8;
  int32_t m_hist_counts[kHist] = {};
  uint32_t m_hist_ms[kHist] = {};
  uint8_t m_hist_n = 0;
  uint8_t m_hist_head = 0;
  int32_t m_ref_counts = 0;
  int32_t m_ref_pos = 0;
  uint32_t m_ref_ms = 0;
  int32_t m_stall_counts = 1;
  bool m_blocked = false;
  uint32_t m_blocked_since = 0;
  bool m_wrong = false;
  uint32_t m_wrong_since = 0;
};

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/arc_controller.cpp`:

```cpp
#include "core/arc_controller.h"

#include "core/motion_config.h"
#include "core/turn_controller.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

void ArcController::begin(const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct,
                          uint16_t cpm, uint32_t now_ms) {
  m_rate.reset();
  m_delta = delta_cdeg;
  m_pos = 0;
  m_dir = delta_cdeg < 0 ? -1 : 1;
  m_last_yaw = t.yaw_cdeg;
  m_radius = radius_cm == 0 ? 1 : radius_cm;
  m_cpm = cpm;
  m_speed = speed_pct;
  m_target_ddps = 0;
  m_last_ms = now_ms;
  m_hist_n = 0;
  m_hist_head = 0;
  const int32_t counts = (t.enc_l + t.enc_r) / 2;
  m_ref_counts = counts;
  m_ref_pos = 0;
  m_ref_ms = now_ms;
  m_stall_counts = cpm / 100 > 0 ? cpm / 100 : 1;  // 1 cm
  m_blocked = false;
  m_wrong = false;
  // timeout: arc length R·|θ| / 5 cm/s + 3 s, |θ| = |delta| / 100 · π / 180
  const int64_t a = delta_cdeg < 0 ? -static_cast<int64_t>(delta_cdeg) : delta_cdeg;
  const uint32_t len_cm = static_cast<uint32_t>(static_cast<int64_t>(m_radius) * a * 31416 / 180000000);
  m_deadline = now_ms + len_cm * 1000u / motion::kArcTimeoutCmPerS + motion::kArcTimeoutBaseMs;
  speedCmS10(counts, now_ms);  // first history entry
}

int32_t ArcController::speedCmS10(int32_t counts, uint32_t now_ms) {
  m_hist_counts[m_hist_head] = counts;
  m_hist_ms[m_hist_head] = now_ms;
  m_hist_head = static_cast<uint8_t>((m_hist_head + 1) % kHist);
  if (m_hist_n < kHist) ++m_hist_n;
  uint8_t pick = static_cast<uint8_t>((m_hist_head + kHist - m_hist_n) % kHist);  // oldest
  for (uint8_t i = 1; i < m_hist_n; ++i) {  // newest entry at least the window back
    const uint8_t j = static_cast<uint8_t>((m_hist_head + kHist - 1 - i) % kHist);
    if (now_ms - m_hist_ms[j] >= motion::kArcSpeedWindowMs) {
      pick = j;
      break;
    }
  }
  const uint32_t dt = now_ms - m_hist_ms[pick];
  const int64_t d = static_cast<int64_t>(counts) - m_hist_counts[pick];
  if (dt == 0 || d <= 0) return 0;  // counts never fall while driving: a drop is an Uno restart (review focus 1)
  return static_cast<int32_t>(d * 1000000 / (static_cast<int64_t>(dt) * m_cpm));
}

ArcStep ArcController::step(const MotionTelem& t, uint32_t now_ms) {
  ArcStep s;
  m_pos += wrapDeltaCdeg(static_cast<int32_t>(t.yaw_cdeg) - static_cast<int32_t>(m_last_yaw));
  m_last_yaw = t.yaw_cdeg;
  const int32_t to_go = m_dir * (m_delta - m_pos);
  const int32_t rate = t.rate_ddps < 0 ? -t.rate_ddps : t.rate_ddps;
  if (to_go <= rate * motion::kLatencyDdpsToCdeg / 10) {  // the heading still to go is covered by the lag
    s.done = true;
    return s;
  }
  const int32_t counts = (t.enc_l + t.enc_r) / 2;
  const int32_t v10 = speedCmS10(counts, now_ms);
  const bool blocked_flag = ((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) != 0;
  if (blocked_flag) {
    if (!m_blocked) m_blocked_since = now_ms;
    m_blocked = true;
    m_ref_ms = now_ms;  // an obstacle ends as blocked, never as stalled
    if (now_ms - m_blocked_since >= motion::kMoveBlockedMs) {
      s.blocked = true;
      return s;
    }
  } else {
    m_blocked = false;
  }
  const int32_t dpos = m_pos - m_ref_pos;
  if (counts - m_ref_counts >= m_stall_counts || dpos >= motion::kArcStallCdeg || dpos <= -motion::kArcStallCdeg) {
    m_ref_counts = counts;
    m_ref_pos = m_pos;
    m_ref_ms = now_ms;
  } else if (now_ms - m_ref_ms >= motion::kArcStallMs) {
    if (t.dist_mm < motion::kMoveObstacleMm) {
      s.blocked = true;
    } else {
      s.stalled = true;
    }
    return s;
  }
  if (static_cast<int32_t>(now_ms - m_deadline) >= 0) {
    s.timeout = true;
    return s;
  }
  uint32_t dt = now_ms - m_last_ms;
  if (dt == 0 || dt > 100) dt = 20;
  m_last_ms = now_ms;
  m_target_ddps = static_cast<int16_t>(m_dir * 573 * v10 / (10 * static_cast<int32_t>(m_radius)));  // 57.3 v / R
  s.w = m_rate.step(m_target_ddps, t.rate_ddps, static_cast<uint16_t>(dt));
  s.v = static_cast<int8_t>(m_speed);
  const bool saturated = s.w >= motion::kRateSaturatedPct || s.w <= -motion::kRateSaturatedPct;
  const bool opposite = (m_dir > 0 && t.rate_ddps < -motion::kRateWrongWayDdps) ||
                        (m_dir < 0 && t.rate_ddps > motion::kRateWrongWayDdps);
  if (saturated && opposite) {
    if (!m_wrong) m_wrong_since = now_ms;
    m_wrong = true;
    if (now_ms - m_wrong_since >= motion::kRateWrongWayMs) {
      s.diverged = true;
      s.v = 0;
      s.w = 0;
    }
  } else {
    m_wrong = false;
  }
  return s;
}

uint8_t ArcController::progressPct() const {
  if (m_delta == 0) return 100;
  const int32_t p = m_pos * 100 / m_delta;
  return static_cast<uint8_t>(p < 0 ? 0 : (p > 100 ? 100 : p));
}

}  // namespace core
}  // namespace cq
```

Check `motion::kLatencyDdpsToCdeg` usage against `turn_controller.cpp` (`grep -n kLatencyDdpsToCdeg src/core/turn_controller.cpp`) and use the same expression it uses if it differs from `rate * k / 10`.

- [ ] **Step 3: Run** — `pio test -e native -f test_arc` → 10 PASS; then `pio test -e native` → all PASS. If `test_arc_left_and_right_end_at_heading` misses by > 2°, print `p.yaw_deg` at the end and the rate at the done frame before changing anything: the done rule (lead = rate × 80 ms) is the spin's, and the plant's lag is 60 ms + 150 ms.

- [ ] **Step 4: Commit** — `feat(bridge): ArcController - gyro-closed arc on ω = v_meas / R (plan L)`.

---

### Task 3: Arcs and pivots in the service, link and router

**Files:**
- Modify: `firmware/esp32/src/core/motion_service.h/.cpp`, `firmware/esp32/src/core/motion_link.cpp`, `firmware/esp32/src/core/motion_router.h/.cpp`
- Test: `firmware/esp32/test/test_motion/test_main.cpp`

**Interfaces:**
- Consumes: `ArcController` (Task 2), `TurnController`.
- Produces: `PrimKind { kNone, kTurn, kRate, kMove, kArc, kPivot }`; `void MotionService::startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm, int32_t id, uint32_t now_ms)`; `void MotionService::startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms)`; `Route routeTurn(uint8_t body_caps, uint16_t radius_cm, bool pivot = false)`.

- [ ] **Step 1: Failing tests** — in `test/test_motion/test_main.cpp` (and the RUN_TESTs):

```cpp
// ---------- Plan L (bridge LLD R8): arcs and pivots ----------
void test_router_arc_and_pivot_need_encoders() {
  const uint8_t enc = kLiteCaps | bit(proto::CapBit::kEncoders);
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(enc, 60)));
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(enc, 0, true)));
  TEST_ASSERT_EQUAL(R(Route::kUnavailable), R(routeTurn(kLiteCaps, 0, true)));
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(kLiteCaps, 0)));  // the spin needs only the gyro
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeTurn(enc | bit(proto::CapBit::kMove), 60)));
}

void test_service_pivot_keeps_inside_track_still() {
  for (int dir = -1; dir <= 1; dir += 2) {
    MotionService svc;
    svc.startPivot(3, 0, dir * 9000, 40, 1, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kPivot), static_cast<int>(svc.kind()));
    const MotionOut o = svc.onTelemetry(0, 0, 20);
    TEST_ASSERT_TRUE(o.send);
    const int l = o.v - o.w, r = o.v + o.w;  // the Uno's plain AUTO mix
    TEST_ASSERT_EQUAL_INT(0, dir > 0 ? l : r);           // inside track stopped
    TEST_ASSERT_TRUE((dir > 0 ? r : l) <= 40);           // outside at most the asked speed
    TEST_ASSERT_TRUE((dir > 0 ? r : l) > 0);
  }
}

void test_service_pivot_speed_floor() {  // Review Focus 4
  MotionService svc;
  svc.startPivot(3, 0, 9000, 12, 1, 0);
  const MotionOut o = svc.onTelemetry(0, 0, 20);
  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, o.w);
  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, o.v);  // outside track 24 %, never a stalled 12 %
}

void test_service_arc_runs_and_cancels() {
  MotionService svc;
  svc.startArc(3, encAt(0), 9000, 60, 40, motion::kCpmDefault, 2, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kArc), static_cast<int>(svc.kind()));
  const MotionOut o = svc.onTelemetry(encAt(30), 20);
  TEST_ASSERT_TRUE(o.send);
  TEST_ASSERT_EQUAL_INT8(40, o.v);
  const MotionOut e = svc.cancel(EndReason::kCancelled);
  TEST_ASSERT_TRUE(e.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kArc), static_cast<int>(e.end.kind));
  TEST_ASSERT_FALSE(e.end.has_moved);
  TEST_ASSERT_EQUAL_INT32(2, e.end.id);
}

void test_link_uno_stall_ends_an_arc_not_a_pivot() {
  {
    LinkOwner owner;
    MotionLink ml{owner};
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);
    ml.service().startArc(kFd, encAt(0), 9000, 60, 40, motion::kCpmDefault, 4, 0);
    ml.onTelemetry(encAt(0), 20);
    const LinkOut o = ml.onEvent(MotionEvent::kBodyStall, 30);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStalled), static_cast<int>(o.end.reason));
  }
  {
    LinkOwner owner;
    MotionLink ml{owner};
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);
    ml.service().startPivot(kFd, 0, 9000, 40, 5, 0);
    ml.onTelemetry(0, 0, 20);
    TEST_ASSERT_FALSE(ml.onEvent(MotionEvent::kBodyStall, 30).event);  // pivots keep the spin's own stall check
    TEST_ASSERT_TRUE(ml.service().active());
  }
}
```

(`kFd` and `encAt` already exist in the file from Plans H / K; add these tests after the Plan K block so they are defined.) Run: `pio test -e native -f test_motion` → build FAIL (`startPivot` / `kPivot` / `routeTurn` arity).

- [ ] **Step 2: Implement**
  - `motion_router.h`: `Route routeTurn(uint8_t body_caps, uint16_t radius_cm, bool pivot = false);  // arc / pivot: body's MOVE, else the bridge with encoders (Plan L)`. `motion_router.cpp`:

```cpp
Route routeTurn(uint8_t body_caps, uint16_t radius_cm, bool pivot) {
  if (radius_cm > 0 || pivot) {  // Plan L: arcs and pivots run in the bridge for a body with encoders
    if (has(body_caps, proto::CapBit::kMove)) return Route::kForward;
    return has(body_caps, proto::CapBit::kEncoders) ? Route::kLocal : Route::kUnavailable;
  }
  return has(body_caps, proto::CapBit::kTurn) ? Route::kForward : Route::kLocal;
}
```

  - `motion_service.h`: `#include "core/arc_controller.h"`; `enum class PrimKind : uint8_t { kNone, kTurn, kRate, kMove, kArc, kPivot };`; public

```cpp
  void startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm,
                int32_t id, uint32_t now_ms);
  void startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms);
```

    private `MotionOut stepArc(const MotionTelem& t, uint32_t now_ms);`, `MotionOut stepPivot(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);`, member `ArcController m_arc;`.
  - `motion_service.cpp`:

```cpp
void MotionService::startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct,
                             uint16_t cpm, int32_t id, uint32_t now_ms) {
  m_kind = PrimKind::kArc;
  m_fd = fd;
  m_id = id;
  m_arc.begin(t, delta_cdeg, radius_cm, speed_pct, cpm, now_ms);
  m_last_telem_ms = now_ms;
}

void MotionService::startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id,
                               uint32_t now_ms) {
  m_kind = PrimKind::kPivot;
  m_fd = fd;
  m_id = id;
  // outside track = 2|w| <= speed; the spin's minimum w still applies (12 % -> outside 24 %)
  uint8_t half = static_cast<uint8_t>(speed_pct / 2);
  if (half < motion::kTurnWMin) half = static_cast<uint8_t>(motion::kTurnWMin);
  m_turn.begin(yaw_cdeg, delta_cdeg, half, now_ms);
  m_last_telem_ms = now_ms;
}

MotionOut MotionService::stepArc(const MotionTelem& t, uint32_t now_ms) {
  const ArcStep s = m_arc.step(t, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.blocked) return finish(EndReason::kBlocked);
  if (s.timeout) return finish(EndReason::kTimeout);
  if (s.diverged) return finish(EndReason::kDiverged);
  MotionOut o;
  o.send = true;
  o.v = s.v;
  o.w = s.w;
  return o;
}

MotionOut MotionService::stepPivot(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.w = s.w;
  o.v = static_cast<int8_t>(s.w < 0 ? -s.w : s.w);  // plain mix: the inside track gets v - |w| = 0
  return o;
}
```

    `onTelemetry` switch gains `case PrimKind::kArc: o = stepArc(t, now_ms); break;` and `case PrimKind::kPivot: o = stepPivot(t.yaw_cdeg, t.rate_ddps, now_ms); break;`; `progressPct` gains `if (m_kind == PrimKind::kArc) return m_arc.progressPct();` and `if (m_kind == PrimKind::kPivot) return m_turn.progressPct();`. Update the file comment (TURN, DRIVE_RATE, MOVE, arc, pivot).
  - `motion_link.cpp` in `onEvent`: `if (m_svc.kind() != PrimKind::kMove && m_svc.kind() != PrimKind::kArc) return LinkOut{};  // spin / pivot / rate keep their own stall checks`.
  - `ws_json.cpp` `primName` stays: kArc and kPivot print `"turn"` (spec H4).

- [ ] **Step 3: Run** — `pio test -e native` → all PASS (existing router tests unchanged: `routeTurn(kLiteCaps, 30)` is still Unavailable because the Lite caps have no encoders).

- [ ] **Step 4: Commit** — `feat(bridge): arcs and pivots in the motion service; Uno stall ends an arc (plan L)`.

---

### Task 4: Minimum spin rate for drive_rate, `limited` in tf

**Files:**
- Modify: `firmware/esp32/src/core/motion_service.h/.cpp`, `firmware/esp32/src/core/ws_json.h/.cpp`
- Test: `firmware/esp32/test/test_motion/test_main.cpp`, `firmware/esp32/test/test_ws_json/test_main.cpp`

**Interfaces:**
- Produces: `bool MotionService::limited() const`, `int16_t MotionService::rateTargetDdps() const`; `PrimitiveView { bool active; PrimKind kind; uint8_t pct; bool limited = false; }`; `tf.primitive` gains `,"limited":true`.

- [ ] **Step 1: Failing tests** — `test_motion`:

```cpp
// Plan L (spec 02 §H6): a spinning drive_rate is never slower than the slowest smooth spin.
void test_rate_clamp_to_minimum_spin() {  // Review Focus 5
  const struct { int8_t v; int16_t asked; int16_t got; bool limited; } cases[] = {
      {0, 50, motion::kSpinMinDdps, true},   {0, -50, -motion::kSpinMinDdps, true}, {0, -10, -motion::kSpinMinDdps, true},
      {0, 0, 0, false},                       {0, 800, 800, false},                  {0, motion::kSpinMinDdps, motion::kSpinMinDdps, false},
      {30, 50, 50, false}};
  for (const auto& c : cases) {
    MotionService svc;
    svc.driveRate(3, c.v, c.asked, 0);
    TEST_ASSERT_EQUAL_INT16(c.got, svc.rateTargetDdps());
    TEST_ASSERT_EQUAL(c.limited, svc.limited());
  }
}
```

  `test_ws_json`:

```cpp
void test_tf_primitive_limited() {
  TankSnapshot s;
  s.fast_valid = true;
  jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kRate, 0, true});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"rate\",\"pct\":0,\"limited\":true}"));
  jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kTurn, 42, false});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"turn\",\"pct\":42}"));
}
```

  Run → build FAIL (`rateTargetDdps`, `limited`, the 4-field `PrimitiveView`).

- [ ] **Step 2: Implement**
  - `motion_service.h`: public `bool limited() const { return m_kind == PrimKind::kRate && m_rate_limited; }` and `int16_t rateTargetDdps() const { return m_rate_target; }`; member `bool m_rate_limited = false;`.
  - `motion_service.cpp` `driveRate`, before `m_rate_v = v;`:

```cpp
  // Plan L (spec 02 §H6): a spin slower than 65 °/s cannot run smoothly - clamp it and say so.
  const int16_t mag = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  m_rate_limited = v == 0 && mag > 0 && mag < motion::kSpinMinDdps;
  if (m_rate_limited) rate_ddps = rate_ddps < 0 ? static_cast<int16_t>(-motion::kSpinMinDdps) : motion::kSpinMinDdps;
```

    (`rate_ddps` is a by-value parameter; `#include "core/motion_config.h"` is already there.)
  - `ws_json.h` `PrimitiveView`: add `bool limited = false;  // Plan L: drive_rate clamped to the slowest smooth spin`.
  - `ws_json.cpp` `jsonTf`: `char progress[64]` and

```cpp
    snprintf(progress, sizeof(progress), ",\"primitive\":{\"kind\":\"%s\",\"pct\":%u%s}", primName(prim.kind),
             static_cast<unsigned>(prim.pct), prim.limited ? ",\"limited\":true" : "");
```

- [ ] **Step 3: Run** — `pio test -e native` → all PASS.
- [ ] **Step 4: Commit** — `feat(bridge): spinning drive_rate clamped to 65 °/s with tf limited flag (plan L)`.

---

### Task 5: `pivot` field and nack with a message

**Files:**
- Modify: `firmware/esp32/src/core/ws_codec.h/.cpp`, `firmware/esp32/src/core/ws_json.h/.cpp`
- Test: `firmware/esp32/test/test_ws_codec/test_main.cpp`, `firmware/esp32/test/test_ws_json/test_main.cpp`

**Interfaces:**
- Produces: `WsCommand.pivot` (bool); `size_t jsonNackMessage(char* out, size_t cap, int32_t id, BridgeReason reason, const char* message)`.

- [ ] **Step 1: Failing tests** — `test_ws_codec`:

```cpp
// Plan L (bridge LLD R8.1): pivot.
void test_turn_pivot_field() {
  WsCommand c = P("{\"t\":\"turn\",\"by\":90,\"pivot\":true}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(c.kind));
  TEST_ASSERT_TRUE(c.pivot);
  TEST_ASSERT_FALSE(P("{\"t\":\"turn\",\"by\":90}").pivot);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"pivot\":true,\"radius_cm\":60}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"pivot\":1}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(P("{\"t\":\"turn\",\"by\":90,\"pivot\":false,\"radius_cm\":60}").kind));
}
```

  `test_ws_json`:

```cpp
void test_nack_with_message() {
  jsonNackMessage(buf, sizeof(buf), 3, BridgeReason::kInvalidCommand, "radius 40 cm: use speed 43-64 %, or pivot");
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"nack\",\"id\":3,\"code\":\"COMMAND_3006\",\"reason\":\"INVALID_COMMAND\","
      "\"message\":\"radius 40 cm: use speed 43-64 %, or pivot\"}",
      buf);
  jsonNackMessage(buf, sizeof(buf), -1, BridgeReason::kInvalidCommand, "say \"hi\"");
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"message\":\"say \\\"hi\\\"\""));  // escaped
}
```

  Run → build FAIL.

- [ ] **Step 2: Implement**
  - `ws_codec.h` `WsCommand`: `bool pivot = false;  // turn: pivot around the inside track (Plan L)`.
  - `ws_codec.cpp`: `Seen` gains `bool pivot = false;`; `readField`, next to `"on"`: `if (strcmp(key, "pivot") == 0) return readBool(r, c.pivot, s.pivot);`; `complete`:

```cpp
    case WsCmdKind::kTurn:  // exactly one of by / to; a pivot has no radius (Plan L)
      return s.mf.by != s.mf.to && c.speed >= motion::kTurnWMin && !(c.pivot && c.radius_cm > 0);
```

  - `ws_json.h`: `// Plan L: nack with its own message (JSON-escaped), e.g. the arc band refusal.` + `size_t jsonNackMessage(char* out, size_t cap, int32_t id, BridgeReason reason, const char* message);`
  - `ws_json.cpp` (after `jsonNack`):

```cpp
size_t jsonNackMessage(char* out, size_t cap, int32_t id, BridgeReason reason, const char* message) {
  char esc[128];
  if (jsonEscape(esc, sizeof(esc), message) == 0 && message[0] != '\0') return 0;
  ReasonInfo r = kBridgeReasons[static_cast<size_t>(reason)];
  r.message = esc;
  return nack(out, cap, id, r);
}
```

    Check `ReasonInfo` (`grep -n "struct ReasonInfo" -A5 src/core/ws_json.cpp`): if `message` is declared `const char* const`, change it to `const char*`. Check `jsonEscape`'s return value (length written); adapt the guard if it returns something else.

- [ ] **Step 3: Run** — `pio test -e native` → all PASS.
- [ ] **Step 4: Commit** — `feat(bridge): turn pivot field and nack with a message (plan L)`.

---

### Task 6: Link task wiring (build + OTA)

**Files:**
- Modify: `firmware/esp32/src/net/link_task.cpp`

**Interfaces:**
- Consumes: Tasks 1–5.

Glue on the ESP32 (no host test runs it): checked by the `bridge` build and the floor session.

- [ ] **Step 1: Implement** — `#include "core/arc_limits.h"`; in `handleTurn`:
  - route: `core::routeTurn(g_link.hello().caps, c.radius_cm, c.pivot)`; the comment above `handleTurn` becomes "turn (LLD R4.2 / R8): spin, arc or pivot — Local on a body without TURN / MOVE; Forward / Unavailable answered UNAVAILABLE for now".
  - after the body-offline / `fast_valid` check:

```cpp
  if (c.radius_cm > 0) {  // Plan L: arcs need trustworthy counts and a radius / speed the tracks can drive
    if ((snap.fast.flags >> static_cast<unsigned>(proto::FastFlagBit::kEncoderFault)) & 1u) {
      nack(fd, id, core::BridgeReason::kUnavailable);
      return;
    }
    const core::ArcBand b = core::arcBand(c.radius_cm);
    if (!b.possible || c.speed < b.lo || c.speed > b.hi) {
      char why[96];
      core::arcRefusal(why, sizeof(why), c.radius_cm);
      sendTo(fd, core::jsonNackMessage(g_json, sizeof(g_json), id, core::BridgeReason::kInvalidCommand, why));
      return;
    }
  }
```

  - replace the `startTurn` line with:

```cpp
  if (c.radius_cm > 0) {
    g_motion.service().startArc(fd, telemOf(snap), delta, c.radius_cm, c.speed, g_cpm, id, now);
  } else if (c.pivot) {
    g_motion.service().startPivot(fd, snap.fast.yaw, delta, c.speed, id, now);
  } else {
    g_motion.service().startTurn(fd, snap.fast.yaw, delta, c.speed, id, now);
  }
```

    (`telemOf` is defined below `motionOwnedBy`; `handleTurn` comes after it — check, and move `telemOf` up if the compiler says it is undeclared.)
  - `telemetry()`: `const core::PrimitiveView pv{svc.active(), svc.kind(), svc.progressPct(), svc.limited()};`
- [ ] **Step 2: Build** — `pio run -e bridge && pio test -e native` → SUCCESS, all PASS.
- [ ] **Step 3: Commit** — `feat(bridge): turn runs arcs and pivots, refuses arcs outside their band (plan L)`.

---

### Task 7: `turn.py` tool

**Files:**
- Create: `tools/smoke/turn.py`, `tools/smoke/tests/test_turn_math.py`

**Interfaces:**
- Consumes: `move.Session` (`pump_until`, `fresh_td`, `counts_per_m`, `close`, `ws`, `next_id`).
- Produces: `integrate(samples: list[tuple[int, float]], counts_per_cm: float) -> tuple[float, float]`, `arc_point(radius_cm: float, deg: float) -> tuple[float, float]`.

- [ ] **Step 1: Failing tests** — `tools/smoke/tests/test_turn_math.py`:

```python
"""Plan L: the path maths of turn.py (bridge LLD R8.5)."""

from __future__ import annotations

import math

from turn import arc_point, integrate


def test_integrate_straight() -> None:
    samples = [(i * 751, 0.0) for i in range(11)]
    x, y = integrate(samples, 75.1)
    assert abs(x - 100.0) < 0.01 and abs(y) < 0.01


def test_integrate_quarter_circle_left() -> None:
    r, n, cpc = 60.0, 900, 75.1
    length = r * math.pi / 2
    samples = [(round(i * length * cpc / n), i * 90.0 / n) for i in range(n + 1)]
    x, y = integrate(samples, cpc)
    assert abs(x - 60.0) < 1.0 and abs(y - 60.0) < 1.0


def test_arc_point() -> None:
    x, y = arc_point(60.0, 90.0)
    assert abs(x - 60.0) < 1e-9 and abs(y - 60.0) < 1e-9
    x, y = arc_point(60.0, -90.0)
    assert abs(x - 60.0) < 1e-9 and abs(y + 60.0) < 1e-9
```

  Run: `cd tools/smoke && uv run pytest -q` → FAIL (`No module named 'turn'`).

- [ ] **Step 2: Implement** — `tools/smoke/turn.py`:

```python
"""Arcs and pivots over WiFi (spec 02 Part H, bridge LLD R8.5).

    uv run python turn.py 90 --radius 60 --speed 40     # arc left 90° on a 60 cm radius
    uv run python turn.py -90 --pivot                    # pivot right 90°

Prints the end heading change and the end position (x forward, y left, cm) from the encoders and the gyro, next to
the geometric point. STOP from the phone or the IR remote ends a turn.
"""

from __future__ import annotations

import argparse
import math
from typing import Any

from move import Session, wrap_deg

TRACK_HALF_CM = 9.5  # B / 2: a pivot turns around the inside track


def integrate(samples: list[tuple[int, float]], counts_per_cm: float) -> tuple[float, float]:
    """Path from (mean counts, yaw °) samples: x forward, y left of the start pose, in cm."""
    x = y = 0.0
    yaw0 = samples[0][1]
    for (c0, a0), (c1, a1) in zip(samples, samples[1:], strict=False):
        ds = (c1 - c0) / counts_per_cm
        h = math.radians(wrap_deg((a0 + a1) / 2 - yaw0)) if abs(a1 - a0) < 180 else math.radians(wrap_deg(a1 - yaw0))
        x += ds * math.cos(h)
        y += ds * math.sin(h)
    return x, y


def arc_point(radius_cm: float, deg: float) -> tuple[float, float]:
    """Where an ideal arc of `deg` (+ left) on `radius_cm` ends, from the start pose."""
    t = math.radians(abs(deg))
    side = 1.0 if deg >= 0 else -1.0
    return radius_cm * math.sin(t), side * radius_cm * (1.0 - math.cos(t))


def run(s: Session, deg: float, radius: int, pivot: bool, speed: int) -> int:
    cpm = s.counts_per_m()
    samples: list[tuple[int, float]] = []

    def keep(m: dict[str, Any]) -> None:
        if m.get("t") == "td":
            samples.append(((int(m["enc"][0]) + int(m["enc"][1])) // 2, float(m["yaw"])))

    start = s.fresh_td()
    keep(start)
    s.next_id += 1
    mid = s.next_id
    cmd: dict[str, Any] = {"t": "turn", "by": deg, "speed": speed, "id": mid}
    if radius:
        cmd["radius_cm"] = radius
    if pivot:
        cmd["pivot"] = True
    s.ws.send(cmd)

    def answered(m: dict[str, Any]) -> bool:
        keep(m)
        return m.get("id") == mid and m.get("t") in ("ack", "nack")

    reply = s.pump_until(answered, 3.0)
    if reply is None or reply.get("t") == "nack":
        print(f"refused: {reply.get('message') if reply else 'no answer'}")
        return 1

    def finished(m: dict[str, Any]) -> bool:
        keep(m)
        return m.get("kind") == "primitive_done" and m.get("id") == mid

    done = s.pump_until(finished, 60.0)
    s.pump_until(lambda m: (keep(m), False)[1], 0.5)  # roll-on
    if done is None:
        print("no primitive_done - press STOP")
        return 1
    x, y = integrate(samples, cpm / 100.0)
    turned = wrap_deg(samples[-1][1] - samples[0][1])
    want = arc_point(radius if radius else TRACK_HALF_CM, deg) if (radius or pivot) else (0.0, 0.0)
    print(
        f"{done.get('reason')}: heading {turned:+.1f} deg (asked {deg:+.0f}), end x {x:.1f} y {y:+.1f} cm "
        f"(ideal x {want[0]:.1f} y {want[1]:+.1f})"
    )
    return 0 if done.get("ok") else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Arc / pivot / spin turn (Plan L)")
    ap.add_argument("deg", type=float, help="+ left, - right")
    ap.add_argument("--radius", type=int, default=0)
    ap.add_argument("--pivot", action="store_true")
    ap.add_argument("--speed", type=int, default=40)
    ap.add_argument("--host", default="conqueror-bridge.local")
    args = ap.parse_args()
    s = Session(args.host)
    s.ws.send({"t": "debug", "on": True, "hz": 50})
    try:
        return run(s, args.deg, args.radius, args.pivot, args.speed)
    finally:
        s.close()


if __name__ == "__main__":
    raise SystemExit(main())
```

  (`Session.__init__` already turns the debug stream on at 10 Hz; the extra `debug` at 50 Hz raises it for a finer path.) If `pyright` objects to the tuple trick in the roll-on lambda, use a small `def` that calls `keep(m)` and returns `False`.

- [ ] **Step 3: Run** — `uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright turn.py tests/test_turn_math.py` → all pass / clean.
- [ ] **Step 4: Commit** — `feat(tools): turn.py - arcs and pivots with the driven path (plan L)`.

---

### Task 8: Floor acceptance, docs, review (owner present)

- [ ] **Step 1:** OTA the bridge (password from `secrets.h`, never printed; log filtered and deleted); `curl /info` shows the commit.
- [ ] **Step 2 (floor; ask first, ~1.5 m clear):** start mark at the tank's centre point and heading line; `turn.py 90 --radius 60` and `turn.py -90 --radius 60`, `turn.py 90 --radius 100` and `-90`; owner measures the end heading and the centre's position. **Pass: ±3°, ±5 cm** of (R, ±R).
- [ ] **Step 3 (floor):** `turn.py 90 --pivot` and `-90 --pivot`: ±3°; the inside track does not move (owner's eye; the inside count change < 1 cm in the `td` stream).
- [ ] **Step 4:** `turn.py 90 --radius 20` → `refused: radius 20 cm too tight (min 31): use pivot`; `turn.py 90 --radius 40` (default 40 %) → `refused: radius 40 cm: use speed 43-64 %, or pivot`.
- [ ] **Step 5 (floor):** a 3 s `drive_rate` spin at 10 °/s (throwaway script) turns smoothly at ~65 °/s and `tf.primitive.limited` is true.
- [ ] **Step 6:** design-notes §71 (results), spec Part H "as run", LLD R8 as built, CLAUDE.md plan list; whole-branch review (fresh reviewer), fix Critical / Important with a failing test first; ask the owner before merging `plan-l`.
