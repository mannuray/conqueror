# Plan I — Encoder features phase 1 (Uno Lite) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Straight driving by encoders plus gyro, a speed-based stall, a heading-hold runaway guard (both stop the tank until the driver commands again) and the plain mix for the bridge's AUTO frames — on the Uno Lite.

**Architecture:** Four small integer domain classes in `firmware/uno/src/domain/lite/` (`TrackSyncLite`, `StallGuardLite`, `RunawayGuardLite`, `MotionLatchLite`) with host tests, plus a `liteMix()` helper for F1.4; the Lite app wires them into the 10 ms control tick and the encoder task; the latch drives `SafetyInputs.stall_mask = 0b11` so only the `SafetySupervisor` still makes motor output. `EncoderHealth` leaves the Lite build. The full firmware gets only F1.4.

**Tech Stack:** C++ (Arduino AVR, `-Os -flto`, no heap, no virtual, no floats in `domain/lite`), Unity host tests (`pio test -e native`), protogen (YAML → generated C++/Python).

**Spec:** `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part F (§F1, owner decisions §F4); LLD `docs/lld/uno-lite-lld.md` §19; design-notes §59–§62.

## Global Constraints
- Uno: no heap, no `virtual`, no `delay()`; domain code never includes `ports/` or `Arduino.h`; only `SafetySupervisor` produces motor output (`PwmOutput`).
- `domain/lite` uses whole numbers only (no `float`).
- Generated files are never edited by hand: change `protocol/defs/*.yaml`, run `cd protocol && uv run protogen write`.
- Safety features (obstacle brake, deadman, link-loss stop, child-play envelope) are never weakened.
- Uno Lite budget: flash must stay ≤ 32 256 B (now 29 248 B), static RAM ≤ 1 560 B (now 1 515 B).
- Owner decisions: straight = encoders **plus** gyro; on stall / fault / runaway **stop both tracks until the driver commands again** (STOP, or command back to v = 0, w = 0); runaway = |error| > 30° and growing with the correction saturated for 0.5 s.
- `max_cps` = 4000 counts/s at PWM 255 (constant in phase 1); encoders count rising edges of channel A only (always increasing, direction from the command), 960 counts / wheel turn.
- Commits: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. **Latch while the stick is held:** a driver who keeps the stick forward after a stall must stay stopped (no stop-start every tick) — test in Task 1 (`test_latch_stays_while_command_held`).
2. **Reverse straight driving:** track sync must slow the *faster* track in reverse too (magnitudes, not signs) — test in Task 2 (`test_sync_reverse_slows_faster_side`).
3. **A slow but moving track is not a stall** (e.g. 20 % of expected speed on carpet) — test in Task 3 (`test_stall_slow_but_moving_is_not_stall`).
4. **Normal heading corrections never trip the runaway guard** (saturated briefly, error shrinking) — test in Task 4 (`test_runaway_normal_correction_never_trips`).
5. **Encoders disabled:** with `encoders_enabled = 0` nothing new may act (no sync, no stall latch) — test in Task 2 (`test_sync_inactive_when_disabled`) and Task 3 (`test_stall_disabled_does_nothing`).

---

### Task 1: HEADING_FAULT event and the motion latch

**Files:**
- Modify: `protocol/defs/events.yaml` (add kind 28)
- Regenerate: `cd protocol && uv run protogen write`
- Create: `firmware/uno/src/domain/lite/motion_latch_lite.h`
- Test: `firmware/uno/test/test_lite_encoders/test_main.cpp` (new)

**Interfaces:**
- Produces: `cq::MotionLatchLite` with `enum class Reason : uint8_t { kNone, kStall, kEncoderFault, kHeading }`, `void trip(Reason r, int8_t side)`, `void clear()`, `void clearIfReleased(int8_t v, int8_t w)`, `bool latched() const`, `Reason reason() const`, `int8_t side() const` (−1 none, 0 left, 1 right), `uint8_t stallMask() const` (0b11 while latched, else 0). `proto::EventKind::kHeadingFault` (28).

- [ ] **Step 1: Add the event** — in `protocol/defs/events.yaml`, after the `SAFETY_OVERRIDE` line, add:
```yaml
- { name: HEADING_FAULT, kind: 28, a: "heading error, whole degrees", doc: "heading hold ran away (error growing past 30° with the correction saturated 0.5 s): both tracks stopped until STOP or the command returns to 0 (spec 02 §F1.3)" }
```
Run: `cd protocol && uv run protogen write && uv run protogen check && uv run pytest -q`
Expected: `wrote … files`, check exit 0, all tests pass.

- [ ] **Step 2: Write the failing tests** — create `firmware/uno/test/test_lite_encoders/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/lite/motion_latch_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_latch_trips_and_masks_both_tracks() {
  MotionLatchLite l;
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL_UINT8(0, l.stallMask());
  l.trip(MotionLatchLite::Reason::kStall, 1);
  TEST_ASSERT_TRUE(l.latched());
  TEST_ASSERT_EQUAL_UINT8(0x03, l.stallMask());  // owner: stop BOTH tracks
  TEST_ASSERT_EQUAL_INT8(1, l.side());
  TEST_ASSERT_EQUAL(static_cast<int>(MotionLatchLite::Reason::kStall), static_cast<int>(l.reason()));
}

void test_latch_stays_while_command_held() {
  MotionLatchLite l;
  l.trip(MotionLatchLite::Reason::kHeading, -1);
  for (int i = 0; i < 100; ++i) l.clearIfReleased(40, 0);  // driver keeps the stick forward
  TEST_ASSERT_TRUE(l.latched());
  l.clearIfReleased(0, 0);  // released
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL_INT8(-1, l.side());
}

void test_latch_clear_by_stop_and_first_trip_wins() {
  MotionLatchLite l;
  l.trip(MotionLatchLite::Reason::kStall, 0);
  l.trip(MotionLatchLite::Reason::kHeading, -1);  // already latched: keeps the first reason
  TEST_ASSERT_EQUAL(static_cast<int>(MotionLatchLite::Reason::kStall), static_cast<int>(l.reason()));
  l.clear();  // STOP
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL(static_cast<int>(MotionLatchLite::Reason::kNone), static_cast<int>(l.reason()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_latch_trips_and_masks_both_tracks);
  RUN_TEST(test_latch_stays_while_command_held);
  RUN_TEST(test_latch_clear_by_stop_and_first_trip_wins);
  return UNITY_END();
}
```

- [ ] **Step 3: Run to see it fail**
Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_encoders`
Expected: build error `domain/lite/motion_latch_lite.h: No such file`.

- [ ] **Step 4: Implement** — create `firmware/uno/src/domain/lite/motion_latch_lite.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Spec 02 §F1 / LLD uno-lite §19: one latch for stall, encoder fault and heading runaway. Owner (2026-10-17): stop
// BOTH tracks until STOP or until the driver's command returns to v = 0, w = 0; the first trip's reason is kept.
class MotionLatchLite {
 public:
  enum class Reason : uint8_t { kNone, kStall, kEncoderFault, kHeading };
  void trip(Reason r, int8_t side) {
    if (m_reason != Reason::kNone) return;
    m_reason = r;
    m_side = side;
  }
  void clear() {
    m_reason = Reason::kNone;
    m_side = -1;
  }
  void clearIfReleased(int8_t v, int8_t w) {
    if (v == 0 && w == 0) clear();
  }
  bool latched() const { return m_reason != Reason::kNone; }
  Reason reason() const { return m_reason; }
  int8_t side() const { return m_side; }
  uint8_t stallMask() const { return latched() ? 0x03 : 0x00; }  // SafetyInputs.stall_mask: both tracks cut

 private:
  Reason m_reason = Reason::kNone;
  int8_t m_side = -1;
};

}  // namespace cq
```

- [ ] **Step 5: Run to see it pass**
Run: `~/.platformio/penv/bin/pio test -e native -f test_lite_encoders`
Expected: `3 test cases: 3 succeeded`.

- [ ] **Step 6: Commit**
```bash
git add protocol firmware/uno/src/generated firmware/esp32/src/generated docs/protocol firmware/uno/src/domain/lite/motion_latch_lite.h firmware/uno/test/test_lite_encoders
git commit -m "feat(uno): motion latch (stop both tracks until released) and HEADING_FAULT event (plan I task 1)"
```

---

### Task 2: TrackSyncLite (F1.1 straight driving by encoders)

**Files:**
- Create: `firmware/uno/src/domain/lite/track_sync_lite.h`, `firmware/uno/src/domain/lite/track_sync_lite.cpp`
- Test: `firmware/uno/test/test_lite_encoders/test_main.cpp`

**Interfaces:**
- Consumes: `cq::Tracks` (`domain/lite/drive_mix_lite.h`: `int16_t l, r` in %).
- Produces: `cq::TrackSyncLite` with `void reset()`, `Tracks apply(Tracks t, bool active, int32_t count_l, int32_t count_r, uint16_t cps_l, uint16_t cps_r)` (returns the corrected tracks; unchanged when inactive), `int16_t correctionPct() const`. Constants `kSyncCountsPerPct = 20`, `kSyncCpsPerPct = 50`, `kSyncMaxPct = 15`, `kSyncMaxCounts = 300`.

- [ ] **Step 1: Write the failing tests** — add to `test_lite_encoders/test_main.cpp` (include `"domain/lite/track_sync_lite.h"`; add the `RUN_TEST` lines):
```cpp
void test_sync_equal_counts_no_change() {
  TrackSyncLite s;
  Tracks t{40, 40};
  t = s.apply(t, true, 1000, 1000, 1200, 1200);  // activation: base taken
  t = s.apply(Tracks{40, 40}, true, 1500, 1500, 1200, 1200);
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
}

void test_sync_slows_faster_left() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 1200, 1200);
  const Tracks t = s.apply(Tracks{40, 40}, true, 200, 100, 1300, 1200);  // left 100 counts ahead, 100 cps faster
  // c = 100/20 + 100/50 = 7 %: left slowed by 3, right sped up by 4
  TEST_ASSERT_EQUAL_INT16(37, t.l);
  TEST_ASSERT_EQUAL_INT16(44, t.r);
  TEST_ASSERT_EQUAL_INT16(7, s.correctionPct());
}

void test_sync_reverse_slows_faster_side() {
  TrackSyncLite s;
  s.apply(Tracks{-40, -40}, true, 0, 0, 1200, 1200);
  const Tracks t = s.apply(Tracks{-40, -40}, true, 200, 100, 1300, 1200);  // counts always rise
  TEST_ASSERT_EQUAL_INT16(-37, t.l);  // magnitude down: slower
  TEST_ASSERT_EQUAL_INT16(-44, t.r);
}

void test_sync_clamped() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 0, 0);
  const Tracks t = s.apply(Tracks{40, 40}, true, 5000, 0, 4000, 0);  // huge error
  TEST_ASSERT_EQUAL_INT16(15, s.correctionPct());
  TEST_ASSERT_EQUAL_INT16(33, t.l);
  TEST_ASSERT_EQUAL_INT16(48, t.r);
}

void test_sync_inactive_when_disabled() {
  TrackSyncLite s;
  const Tracks t = s.apply(Tracks{40, 30}, false, 500, 0, 2000, 0);
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(30, t.r);
  TEST_ASSERT_EQUAL_INT16(0, s.correctionPct());
}

void test_sync_rebases_after_inactive() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 1200, 1200);
  s.apply(Tracks{40, 40}, true, 300, 100, 1200, 1200);
  s.apply(Tracks{40, 40}, false, 300, 100, 1200, 1200);              // steering: off
  const Tracks t = s.apply(Tracks{40, 40}, true, 300, 100, 1200, 1200);  // new straight stretch: new base
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
}
```

- [ ] **Step 2: Run to see it fail** — Run: `~/.platformio/penv/bin/pio test -e native -f test_lite_encoders`. Expected: build error `track_sync_lite.h: No such file`.

- [ ] **Step 3: Implement** — `track_sync_lite.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/lite/drive_mix_lite.h"

namespace cq {

// Spec 02 §F1.1 / LLD uno-lite §19.1: while driving straight keep both tracks' counted distance equal (fast inner
// loop); the gyro heading hold keeps running on top (slow outer loop, slip). Counts always rise (channel A rising
// edges), so the correction works on magnitudes: the faster side is slowed, the slower one sped up.
class TrackSyncLite {
 public:
  static constexpr int16_t kSyncCountsPerPct = 20;  // 20 counts ahead -> 1 %
  static constexpr int16_t kSyncCpsPerPct = 50;     // 50 counts/s faster -> 1 %
  static constexpr int16_t kSyncMaxPct = 15;
  static constexpr int32_t kSyncMaxCounts = 300;    // anti-windup on the distance term
  void reset() { m_active = false; m_c = 0; }
  Tracks apply(Tracks t, bool active, int32_t count_l, int32_t count_r, uint16_t cps_l, uint16_t cps_r);
  int16_t correctionPct() const { return m_c; }

 private:
  bool m_active = false;
  int32_t m_base_l = 0;
  int32_t m_base_r = 0;
  int16_t m_c = 0;
};

}  // namespace cq
```
`track_sync_lite.cpp`:
```cpp
#include "domain/lite/track_sync_lite.h"

namespace cq {
namespace {
int16_t towardZero(int16_t v, int16_t d) { return v >= 0 ? static_cast<int16_t>(v - d) : static_cast<int16_t>(v + d); }
}  // namespace

Tracks TrackSyncLite::apply(Tracks t, bool active, int32_t count_l, int32_t count_r, uint16_t cps_l, uint16_t cps_r) {
  if (!active) {
    reset();
    return t;
  }
  if (!m_active) {
    m_active = true;
    m_base_l = count_l;
    m_base_r = count_r;
  }
  int32_t ahead = (count_l - m_base_l) - (count_r - m_base_r);  // > 0: left has gone further
  if (ahead > kSyncMaxCounts) ahead = kSyncMaxCounts;
  if (ahead < -kSyncMaxCounts) ahead = -kSyncMaxCounts;
  int32_t c = ahead / kSyncCountsPerPct + (static_cast<int32_t>(cps_l) - cps_r) / kSyncCpsPerPct;
  if (c > kSyncMaxPct) c = kSyncMaxPct;
  if (c < -kSyncMaxPct) c = -kSyncMaxPct;
  m_c = static_cast<int16_t>(c);
  const int16_t half = static_cast<int16_t>(m_c / 2);    // the faster side gives up half
  const int16_t rest = static_cast<int16_t>(m_c - half);  // the slower side gains the rest
  t.l = towardZero(t.l, half);
  t.r = towardZero(t.r, static_cast<int16_t>(-rest));
  return t;
}

}  // namespace cq
```

- [ ] **Step 4: Run to see it pass** — Expected: `9 test cases: 9 succeeded`.

- [ ] **Step 5: Commit**
```bash
git add firmware/uno/src/domain/lite/track_sync_lite.* firmware/uno/test/test_lite_encoders
git commit -m "feat(uno): TrackSyncLite - encoder distance sync for straight driving (plan I task 2)"
```

---

### Task 3: StallGuardLite (F1.2 stall by speed, encoder fault)

**Files:**
- Create: `firmware/uno/src/domain/lite/stall_guard_lite.h`, `firmware/uno/src/domain/lite/stall_guard_lite.cpp`
- Test: `firmware/uno/test/test_lite_encoders/test_main.cpp`

**Interfaces:**
- Produces: `cq::StallGuardLite` with `struct Inputs { int16_t pwm_l, pwm_r; uint8_t keep_l, keep_r; uint16_t cps_l, cps_r; int16_t yaw_rate_dps; bool enabled; }`, `enum class Result : uint8_t { kNone, kStall, kFault }`, `Result update(const Inputs& in, uint32_t now_ms, int8_t& side)`, `bool faulted(uint8_t side) const`, `void reset()`; constants `kMaxCps = 4000`, `kMargin = 15`, `kSlowPct = 15`, `kDetectMs = 500`.

- [ ] **Step 1: Write the failing tests** (include `"domain/lite/stall_guard_lite.h"`; add `RUN_TEST`s):
```cpp
namespace {
StallGuardLite::Inputs drive(int16_t pl, int16_t pr, uint16_t cl, uint16_t cr, int16_t yaw = 0) {
  return StallGuardLite::Inputs{pl, pr, 90, 90, cl, cr, yaw, true};
}
// expected cps at pwm 130, keep 90: 4000 * 40 / 165 = 969; 15 % = 145
StallGuardLite::Result run(StallGuardLite& g, const StallGuardLite::Inputs& in, uint32_t ms, int8_t& side) {
  StallGuardLite::Result r = StallGuardLite::Result::kNone;
  for (uint32_t t = 0; t <= ms && r == StallGuardLite::Result::kNone; t += 20) r = g.update(in, t, side);
  return r;
}
}  // namespace

void test_stall_held_track_trips_in_half_a_second() {
  StallGuardLite g;
  int8_t side = -1;
  // right held: creeping at 40 counts/s (< 145), tank turning toward it (yaw -25 °/s)
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kNone),
                    static_cast<int>(g.update(drive(130, 130, 969, 40, -25), 0, side)));
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kNone),
                    static_cast<int>(g.update(drive(130, 130, 969, 40, -25), 480, side)));
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kStall),
                    static_cast<int>(g.update(drive(130, 130, 969, 40, -25), 500, side)));
  TEST_ASSERT_EQUAL_INT8(1, side);
}

void test_stall_slow_but_moving_is_not_stall() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kNone),
                    static_cast<int>(run(g, drive(130, 130, 969, 200, -5), 2000, side)));  // 20 % of expected
}

void test_stall_dead_encoder_while_straight_is_fault() {
  StallGuardLite g;
  int8_t side = -1;
  // left reads 0 while the tank drives straight (right moving, no yaw): broken encoder, not a stall
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kFault),
                    static_cast<int>(run(g, drive(130, 130, 0, 969, 0), 600, side)));
  TEST_ASSERT_EQUAL_INT8(0, side);
  TEST_ASSERT_TRUE(g.faulted(0));
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kNone),
                    static_cast<int>(run(g, drive(130, 130, 0, 969, 0), 600, side)));  // no repeat
}

void test_stall_below_keep_is_not_driven() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kNone),
                    static_cast<int>(run(g, drive(100, 100, 0, 0, 0), 2000, side)));  // 100 < keep + 15
}

void test_stall_disabled_does_nothing() {
  StallGuardLite g;
  int8_t side = -1;
  StallGuardLite::Inputs in = drive(130, 130, 0, 0, 0);
  in.enabled = false;
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kNone), static_cast<int>(run(g, in, 2000, side)));
}

void test_stall_both_held_is_stall() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(static_cast<int>(StallGuardLite::Result::kStall),
                    static_cast<int>(run(g, drive(130, 130, 0, 0, 0), 600, side)));  // nothing moves: stall
}
```

- [ ] **Step 2: Run to see it fail** — Expected: build error `stall_guard_lite.h: No such file`.

- [ ] **Step 3: Implement** — `stall_guard_lite.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Spec 02 §F1.2 / LLD uno-lite §19.1: a driven side is stalled when its speed stays below 15 % of the speed expected
// for its PWM for 0.5 s (replaces "no edge at all": a held track still creeps at 960 counts / wheel turn). A side that
// reads 0 while the tank clearly drives straight (other side moving, both PWMs same sign, |yaw| < 10 °/s) is a
// broken encoder instead (fault, once, checks off for that side).
class StallGuardLite {
 public:
  struct Inputs {
    int16_t pwm_l;  // applied last tick
    int16_t pwm_r;
    uint8_t keep_l;  // deadband keep PWM for the current direction
    uint8_t keep_r;
    uint16_t cps_l;
    uint16_t cps_r;
    int16_t yaw_rate_dps;
    bool enabled;  // encoders_enabled
  };
  enum class Result : uint8_t { kNone, kStall, kFault };
  static constexpr uint16_t kMaxCps = 4000;  // counts/s at PWM 255 (phase 1 constant, §59)
  static constexpr uint8_t kMargin = 15;
  static constexpr uint8_t kSlowPct = 15;
  static constexpr uint16_t kDetectMs = 500;
  Result update(const Inputs& in, uint32_t now_ms, int8_t& side);
  bool faulted(uint8_t s) const { return (m_faulted >> s) & 1u; }
  void reset() { m_slow = 0; }

 private:
  uint8_t m_slow = 0;  // bit per side: slow since m_since[s]
  uint8_t m_faulted = 0;
  uint32_t m_since[2] = {0, 0};
};

}  // namespace cq
```
`stall_guard_lite.cpp`:
```cpp
#include "domain/lite/stall_guard_lite.h"

namespace cq {
namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
}  // namespace

StallGuardLite::Result StallGuardLite::update(const Inputs& in, uint32_t now_ms, int8_t& side) {
  if (!in.enabled) {
    m_slow = 0;
    return Result::kNone;
  }
  const int16_t pwm[2] = {in.pwm_l, in.pwm_r};
  const uint8_t keep[2] = {in.keep_l, in.keep_r};
  const uint16_t cps[2] = {in.cps_l, in.cps_r};
  bool slow[2] = {false, false};
  for (uint8_t s = 0; s < 2; ++s) {
    const int16_t mag = absI(pwm[s]);
    if (faulted(s) || mag < keep[s] + kMargin || keep[s] >= 255) continue;
    const uint32_t expected = static_cast<uint32_t>(kMaxCps) * static_cast<uint32_t>(mag - keep[s]) / (255u - keep[s]);
    slow[s] = static_cast<uint32_t>(cps[s]) * 100u < expected * kSlowPct;
  }
  const bool straight = (in.pwm_l > 0 && in.pwm_r > 0) || (in.pwm_l < 0 && in.pwm_r < 0);
  for (uint8_t s = 0; s < 2; ++s) {
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    if (!slow[s]) {
      m_slow = static_cast<uint8_t>(m_slow & ~bit);
      continue;
    }
    if (!(m_slow & bit)) {
      m_slow = static_cast<uint8_t>(m_slow | bit);
      m_since[s] = now_ms;
    }
    if (now_ms - m_since[s] < kDetectMs) continue;
    m_slow = 0;
    side = static_cast<int8_t>(s);
    const uint8_t other = static_cast<uint8_t>(1 - s);
    const bool other_moving = !slow[other] && cps[other] > 0;
    const int16_t yaw = absI(in.yaw_rate_dps);
    if (cps[s] == 0 && straight && other_moving && yaw < 10) {
      m_faulted = static_cast<uint8_t>(m_faulted | bit);
      return Result::kFault;
    }
    return Result::kStall;
  }
  return Result::kNone;
}

}  // namespace cq
```

- [ ] **Step 4: Run to see it pass** — Expected: `15 test cases: 15 succeeded`.

- [ ] **Step 5: Commit**
```bash
git add firmware/uno/src/domain/lite/stall_guard_lite.* firmware/uno/test/test_lite_encoders
git commit -m "feat(uno): StallGuardLite - stall by speed, encoder fault on clear evidence (plan I task 3)"
```

---

### Task 4: RunawayGuardLite (F1.3) and the heading-hold error

**Files:**
- Create: `firmware/uno/src/domain/lite/runaway_guard_lite.h` (header-only)
- Modify: `firmware/uno/src/domain/lite/heading_hold_lite.h/.cpp` (expose the last error)
- Test: `firmware/uno/test/test_lite_encoders/test_main.cpp`

**Interfaces:**
- Consumes: `HeadingHoldLite::holding()`.
- Produces: `int32_t HeadingHoldLite::errorCdeg() const` (last held-heading error, 0.01°, 0 when not holding); `cq::RunawayGuardLite` with `bool step(bool holding, int32_t err_cdeg, int8_t w_ctrl)` (one 10 ms tick; true once when tripping), `void reset()`; constants `kErrCdeg = 3000`, `kTicks = 50`.

- [ ] **Step 1: Write the failing tests** (include `"domain/lite/runaway_guard_lite.h"`):
```cpp
void test_runaway_reversed_motor_trips() {
  RunawayGuardLite g;
  bool tripped = false;
  int32_t err = 3100;
  for (int i = 0; i < 60 && !tripped; ++i) {
    tripped = g.step(true, err, 100);  // saturated correction, error growing 1° per tick
    err += 100;
  }
  TEST_ASSERT_TRUE(tripped);
}

void test_runaway_normal_correction_never_trips() {
  RunawayGuardLite g;
  int32_t err = 4000;  // big error after a bump, shrinking under a saturated correction
  for (int i = 0; i < 200; ++i) {
    TEST_ASSERT_FALSE(g.step(true, err, err > 1000 ? 100 : 40));
    if (err > 0) err -= 30;
  }
}

void test_runaway_needs_saturation_and_holding() {
  RunawayGuardLite g;
  for (int i = 0; i < 100; ++i) TEST_ASSERT_FALSE(g.step(true, 3500 + i * 50, 80));  // not saturated
  for (int i = 0; i < 100; ++i) TEST_ASSERT_FALSE(g.step(false, 3500 + i * 50, 100));  // not holding
}
```

- [ ] **Step 2: Run to see it fail** — Expected: build error `runaway_guard_lite.h: No such file`.

- [ ] **Step 3: Implement** — `runaway_guard_lite.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Spec 02 §F1.3 / LLD uno-lite §19.1: the heading hold made things worse - |error| above 30° and larger than when the
// correction saturated, with the correction saturated for 0.5 s (50 control ticks). A reversed motor (design-notes
// §60) turns the hold into positive feedback; this stops it.
class RunawayGuardLite {
 public:
  static constexpr int32_t kErrCdeg = 3000;
  static constexpr uint8_t kTicks = 50;
  bool step(bool holding, int32_t err_cdeg, int8_t w_ctrl) {
    const int32_t e = err_cdeg < 0 ? -err_cdeg : err_cdeg;
    const bool saturated = w_ctrl >= 100 || w_ctrl <= -100;
    if (!holding || !saturated || e <= kErrCdeg) {
      m_ticks = 0;
      return false;
    }
    if (m_ticks == 0) m_start = e;
    if (++m_ticks < kTicks) return false;
    const bool grew = e > m_start;
    m_ticks = 0;
    return grew;
  }
  void reset() { m_ticks = 0; }

 private:
  uint8_t m_ticks = 0;
  int32_t m_start = 0;
};

}  // namespace cq
```
In `heading_hold_lite.h` add a public `int32_t errorCdeg() const { return m_err; }` and a private `int32_t m_err = 0;`. In `heading_hold_lite.cpp`: set `m_err = 0;` in `reset()` and wherever `teleop` returns before holding; after the wrap of `err` in the holding branch add `m_err = err;`.

- [ ] **Step 4: Run to see it pass** — Run `~/.platformio/penv/bin/pio test -e native -f test_lite_encoders -f test_lite_motion`. Expected: all pass (18 in test_lite_encoders; test_lite_motion unchanged).

- [ ] **Step 5: Commit**
```bash
git add firmware/uno/src/domain/lite/runaway_guard_lite.h firmware/uno/src/domain/lite/heading_hold_lite.* firmware/uno/test/test_lite_encoders
git commit -m "feat(uno): RunawayGuardLite - heading hold growing past 30 deg while saturated (plan I task 4)"
```

---

### Task 5: Plain mix for src AUTO (F1.4), Lite helper + full firmware

**Files:**
- Modify: `firmware/uno/src/domain/lite/drive_mix_lite.h` (+ `liteMix` declaration), `firmware/uno/src/domain/lite/drive_mix_lite.cpp`
- Modify: `firmware/uno/src/app/control_task.cpp:345-350` (full firmware)
- Test: `firmware/uno/test/test_lite_motion/test_main.cpp`

**Interfaces:**
- Produces: `Tracks liteMix(int8_t v, int8_t w_steer, int8_t w_ctrl, bool auto_src, uint8_t turn_full_pct)`.

- [ ] **Step 1: Write the failing test** (in `test_lite_motion`):
```cpp
// Spec 02 §F1.4: the bridge's AUTO frames use the plain mix (L = v - w, R = v + w); humans keep the angle mix.
void test_auto_frames_use_plain_mix() {
  Tracks t = liteMix(30, 30, 0, true, 100);   // AUTO arc: steering passed as a plain correction
  TEST_ASSERT_EQUAL_INT16(0, t.l);
  TEST_ASSERT_EQUAL_INT16(60, t.r);
  t = liteMix(30, 30, 0, false, 100);          // human: angle mix (45° -> outer = stick distance 42)
  TEST_ASSERT_EQUAL_INT16(42, t.r);
  TEST_ASSERT_TRUE(t.l > 0);
}
```

- [ ] **Step 2: Run to see it fail** — Expected: `use of undeclared identifier 'liteMix'`.

- [ ] **Step 3: Implement** — in `drive_mix_lite.h` after `mixLite`:
```cpp
// Spec 02 §F1.4: the bridge motion service's frames (src AUTO) are a controller's output - plain mix; the driver's
// stick keeps the angle mix (§A4.1).
Tracks liteMix(int8_t v, int8_t w_steer, int8_t w_ctrl, bool auto_src, uint8_t turn_full_pct);
```
In `drive_mix_lite.cpp` after `mixLite`:
```cpp
Tracks liteMix(int8_t v, int8_t w_steer, int8_t w_ctrl, bool auto_src, uint8_t turn_full_pct) {
  if (!auto_src) return mixLite(v, w_steer, w_ctrl, turn_full_pct);
  int16_t w = static_cast<int16_t>(w_steer + w_ctrl);
  w = clampI(w, -100, 100);
  return mixLite(v, 0, static_cast<int8_t>(w), turn_full_pct);
}
```
Full firmware `control_task.cpp`, replace
```cpp
      if (imuUsable() && g_app.heading.holding()) {
        w = out;
      } else {
        w_steer = out;
      }
```
with
```cpp
      if ((imuUsable() && g_app.heading.holding()) || g_app.teleop_auto) {
        w = out;  // heading hold, or the bridge's AUTO frame: plain mix (spec 02 §F1.4)
      } else {
        w_steer = out;
      }
```

- [ ] **Step 4: Run** — `~/.platformio/penv/bin/pio test -e native` (all suites) and `~/.platformio/penv/bin/pio run -e mega`. Expected: all pass, Mega builds.

- [ ] **Step 5: Commit**
```bash
git add firmware/uno/src/domain/lite/drive_mix_lite.* firmware/uno/src/app/control_task.cpp firmware/uno/test/test_lite_motion
git commit -m "feat(uno): plain mix for the bridge's AUTO frames, Lite and full (plan I task 5)"
```

---

### Task 6: Wire phase 1 into the Uno Lite app

**Files:**
- Modify: `firmware/uno/src/lite/lite_app.h` (replace `EncoderHealth enc_health;` by the four objects)
- Modify: `firmware/uno/src/lite/lite_sensors.cpp` (`encodersTask`)
- Modify: `firmware/uno/src/lite/lite_control.cpp` (`driveTargets`, `safetyGate`, `publish`, `controlTask`, `kMaxCps`)
- Modify: `firmware/uno/src/lite/lite_routes.cpp` (STOP clears the latch)
- Modify: `firmware/uno/platformio.ini` (`-<…encoder_health.cpp>` from the Uno filter)

**Interfaces:**
- Consumes: Tasks 1–5 (`MotionLatchLite`, `TrackSyncLite`, `StallGuardLite`, `RunawayGuardLite`, `liteMix`, `HeadingHoldLite::errorCdeg`, `proto::EventKind::kHeadingFault`).

- [ ] **Step 1: State** — in `lite_app.h` include the four headers and replace the `enc_health` member with:
```cpp
  StallGuardLite stall;     // spec 02 §F1.2 (only with encoders enabled)
  TrackSyncLite sync;       // §F1.1
  RunawayGuardLite runaway; // §F1.3
  MotionLatchLite latch;    // owner: stop both tracks until released (§F4)
```

- [ ] **Step 2: Encoder task** — in `lite_sensors.cpp` `encodersTask()`, replace everything after `if (!g_lite.encoders_enabled) return;` with:
```cpp
  // Spec 02 §F1.2: stall by speed / encoder fault -> event + latch (stop both tracks until released).
  const LiteSettings& st = g_lite.settings.values();
  const int16_t out_l = g_lite.fast.pwm_l, out_r = g_lite.fast.pwm_r;
  const StallGuardLite::Inputs in{out_l, out_r, st.db_keep[out_l < 0 ? 1 : 0], st.db_keep[out_r < 0 ? 3 : 2],
                                  g_lite.enc_l.cps(), g_lite.enc_r.cps(),
                                  static_cast<int16_t>(g_lite.yaw.rateDdps() / 10), true};
  int8_t side = -1;
  switch (g_lite.stall.update(in, port::clock::nowMs(), side)) {
    case StallGuardLite::Result::kStall:
      g_lite.events.push(proto::EventKind::kStall, side);
      g_lite.latch.trip(MotionLatchLite::Reason::kStall, side);
      break;
    case StallGuardLite::Result::kFault:
      g_lite.events.push(proto::EventKind::kEncoderFault, side);
      g_lite.latch.trip(MotionLatchLite::Reason::kEncoderFault, side);
      break;
    case StallGuardLite::Result::kNone: break;
  }
```
(If `encoders_enabled` is 0 the function already returned: no stall checks, as before.)

- [ ] **Step 3: Control tick** — in `lite_control.cpp`:
  - change `constexpr uint16_t kMaxCps = 2500;` to `constexpr uint16_t kMaxCps = StallGuardLite::kMaxCps;  // 4000 at PWM 255 (§59)`;
  - at the start of `controlTask()` (before `driveTargets`): `g_lite.latch.clearIfReleased(g_lite.teleop_v, g_lite.teleop_w);`
  - in `driveTargets()` replace `Tracks t = mixLite(v, w_steer, w_ctrl, s.turn_full);` with:
```cpp
  if (g_lite.runaway.step(g_lite.heading.holding(), g_lite.heading.errorCdeg(), w_ctrl)) {  // §F1.3
    g_lite.events.push(proto::EventKind::kHeadingFault, static_cast<int16_t>(g_lite.heading.errorCdeg() / 100));
    g_lite.latch.trip(MotionLatchLite::Reason::kHeading, -1);
  }
  Tracks t = liteMix(v, w_steer, w_ctrl, g_lite.teleop_auto, s.turn_full);  // §F1.4
  const bool straight = g_lite.encoders_enabled && !g_lite.stall.faulted(0) && !g_lite.stall.faulted(1) &&
                        w_steer == 0 && t.l != 0 && t.r != 0 && ((t.l > 0) == (t.r > 0)) && !g_lite.latch.latched();
  t = g_lite.sync.apply(t, straight, g_lite.enc_l.count(), g_lite.enc_r.count(), g_lite.enc_l.cps(),
                        g_lite.enc_r.cps());  // §F1.1: encoders inside, gyro outside
```
  - in `safetyGate()` replace `const uint8_t stall_mask = g_lite.encoders_enabled ? g_lite.enc_health.stalledMask() : 0;  // §D5.2` with `const uint8_t stall_mask = g_lite.latch.stallMask();  // §F1 / §F4: both tracks while latched`;
  - in `publish()` replace the three `enc_health` flag lines with:
```cpp
  const bool stalled = g_lite.latch.reason() == MotionLatchLite::Reason::kStall;
  setFast(proto::FastFlagBit::kStallL, stalled && g_lite.latch.side() == 0);
  setFast(proto::FastFlagBit::kStallR, stalled && g_lite.latch.side() == 1);
  setFast(proto::FastFlagBit::kEncoderFault, g_lite.stall.faulted(0) || g_lite.stall.faulted(1));
```
  - if `req_pwm_l` / `req_pwm_r` are now unused, remove them from `lite_app.h` and `safetyGate()`.

- [ ] **Step 4: STOP clears the latch** — in `lite_routes.cpp` `case F::kStop:` add `g_lite.latch.clear();` after `g_lite.teleop_w = 0;`. In `lite_control.cpp` IR handling where `o.stop` calls `zeroTeleop()`, also call `g_lite.latch.clear();`.

- [ ] **Step 5: Build filter** — in `platformio.ini` `[env:uno]` remove `+<domain/estimation/encoder_health.cpp>`.

- [ ] **Step 6: Build and size**
Run: `cd firmware/uno && ~/.platformio/penv/bin/pio run -e uno -e mega && ~/.platformio/penv/bin/pio test -e native`
Expected: both build; `Flash` ≤ 32 256 B and `RAM` ≤ 1 560 B for `uno` (record the numbers); all host tests pass.

- [ ] **Step 7: Commit**
```bash
git add firmware/uno
git commit -m "feat(uno): wire track sync, stall guard, runaway guard and the latch into the Lite (plan I task 6)"
```

---

### Task 7: Tank checks (owner present) and record

**Files:**
- Modify: `docs/design-notes.md` (new section with the results), `docs/lld/uno-lite-lld.md` §19 (measured budget, tuned constants)

- [ ] **Step 1: Flash** — Uno via USB (S1 to upload, then back; unplug USB). Bridge unchanged.
- [ ] **Step 2: Straight line (floor, 3 m tape)** — phone or IR forward at 40 %, record with `cd tools/smoke && uv run python td_record.py --hz 50 --seconds 8 --out straight.csv`. Pass: owner-measured drift ≤ 3 cm over 3 m; `enc` L/R within 2 %. If not: tune `kSyncCountsPerPct` / `kSyncCpsPerPct` (one change at a time, record each).
- [ ] **Step 3: Held track (stand or floor)** — forward 25 %, hold the right track with wood / cloth after 2 s. Pass: within ~0.5 s a `stall` (or, on a stand with the track fully stopped, `encoder_fault`) event, **both tracks stop**, and they stay stopped while the stick is held; releasing the stick (or STOP) frees it.
- [ ] **Step 4: Unplugged encoder** — battery off, unplug the left encoder A wire, drive straight on the floor: `encoder_fault` side 0, both stop; plug back + reboot.
- [ ] **Step 5: Runaway guard** — build once with `kDirLeftForwardHigh = kDirRightForwardHigh = true` (the wrong way for these motors, §60), floor, forward 25 % with the script safety stop: pass = `heading_fault` and both stop within ~0.6 s of saturation; re-flash the normal build.
- [ ] **Step 6: Bridge arc** — `turn by 60` with `radius_cm` is phase 2; here check F1.4 with `drive_rate v 30 / 20 °/s` (the §58 test): heading rate ≈ 20 °/s (was ~10).
- [ ] **Step 7: Record** — results in design-notes (new §) and the LLD; commit `docs: plan I floor results`.

---

### Task 8: Final review and merge
- [ ] Final whole-branch review by a fresh reviewer (most capable model); fix Critical / Important with a failing test first; ledger minors.
- [ ] Ask the owner before merging `encoder-plan` to main; then delete the branch.
