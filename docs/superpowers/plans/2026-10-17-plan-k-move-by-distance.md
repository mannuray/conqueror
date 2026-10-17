# Plan K — MOVE by distance + counts_per_m calibration (bridge) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ESP32 bridge runs `{"t":"move","dist_cm":±n,"speed":%}` on the Uno Lite by encoder distance (slowing to a crawl over the last 10 cm), stores a calibrated `counts_per_m`, and a laptop tool drives and calibrates it.

**Architecture:** A new `MoveController` joins `TurnController` / `RateLoop` inside the bridge's `MotionService`; on every `TELEM_FAST` (50 Hz) it outputs `DRIVE {src AUTO, v, w = 0}` — the Uno keeps the straight driving and every safety rule. `counts_per_m` lives in NVS (`MotionStore`) with a `calib` WebSocket command. `tools/smoke/move.py` sends moves, reads the roll-on from the debug stream and calibrates on a tape.

**Tech Stack:** C++17 ESP32-S3 Arduino (PlatformIO `bridge` / `bridge_ota` envs), Unity host tests (`pio test -e native`), Python 3.12 + uv (ruff, pyright strict, pytest).

**Spec:** `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part G (and §E3 for the shared motion rules); LLD: `docs/lld/esp32-bridge-lld.md` Revision 7.

## Global Constraints

- Bridge only: **no Uno firmware change, no `protocol/defs` change, no protocol-version change** (stays 3).
- `move`: `dist_cm` integer, 1 ≤ |n| ≤ 500, + forward, − reverse; `speed` integer 10…100, default 40; any `heading` key → `INVALID_COMMAND`.
- `counts_per_m` integer 3000…15000, default **7560**; stored in NVS namespace `cq_motion`, key `cpm`.
- Profile: cruise at `speed`; over the last **10 cm** fall linearly to the crawl **15 %** (never below it; speed < 15 → speed); stop when the counted distance reaches the target.
- Ends: `done` (ok) / `stalled` (Uno stall event or < 1 cm in 1.5 s) / `encoder_fault` (Uno event) / `blocked` (blocked flag ≥ 2 s) / `timeout` (|cm| / 5 cm/s + 3 s) / `cancelled` / `stop` / `not_owner` / `link`; `primitive_done` adds `"moved_cm"` (one decimal) for moves.
- Turn and drive_rate behaviour unchanged: all existing host tests stay green.
- Safety features are never weakened: moves are ordinary DRIVE frames; every end sends STOP while the client still owns the tank (not on `not_owner`).
- Bridge code: no heap in the control path, whole numbers in the controller, `-Wall -Wextra -Werror` (native env).
- Python: 3.12, type hints everywhere, ruff + pyright strict clean.
- Commits: Conventional Commits, each ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; work on branch **`plan-k`**; ask the owner before merging to main.
- Hardware steps only with the owner present, STOP within reach; stand first (tracks off the ground), floor only when a step says floor.
- Never print `include/secrets.h` values; OTA logs are filtered for the password and deleted.

## Review Focus

1. **The Uno restarts mid-move** (its encoder counts jump back to 0): the move must end (`stalled` within ~1.5 s, or `link`), never keep driving or report `done` — test in Task 2 (`test_move_counts_reset_ends_stalled`).
2. **An obstacle appears briefly** (blocked < 2 s) and goes away: the move carries on and ends `done` — test in Task 2 (`test_move_brief_block_continues`).
3. **Slowest allowed speed (10 %)**, below the crawl: v must never drop to 0 before the target — test in Task 2 (`test_move_speed_below_crawl_never_zero`).
4. **A 1 cm move at 100 %**: shorter than the slow zone, must stay slow the whole way (≤ 23 %), not jump to 100 % — test in Task 2 (`test_move_short_move_stays_slow`).
5. **`speed` limits differ per command** (turn ≥ 12, move ≥ 10): `turn speed 10` stays invalid while `move speed 10` is valid — test in Task 4 (`test_speed_limits_per_command`).

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/esp32/src/core/motion_config.h` (modify) | MOVE tuning constants, `counts_per_m` limits, `validCpm` |
| `firmware/esp32/src/core/move_controller.h/.cpp` (create) | `MotionTelem`, `MoveStep`, `MoveController` (distance, profile, ends) |
| `firmware/esp32/src/core/motion_service.h/.cpp` (modify) | `PrimKind::kMove`, new `EndReason`s, `PrimitiveEnd.moved_mm`, `startMove`, `onTelemetry(MotionTelem)` |
| `firmware/esp32/src/core/motion_link.h/.cpp` (modify) | `onTelemetry(MotionTelem)`, `MotionEvent::kBodyStall / kBodyEncoderFault` |
| `firmware/esp32/src/core/motion_router.cpp` (modify) | `routeMove`: encoders → Local |
| `firmware/esp32/src/core/ws_codec.h/.cpp` (modify) | `move`, `calib` parsing; per-command speed limits |
| `firmware/esp32/src/core/ws_json.h/.cpp` (modify) | `primitive_done` move / `moved_cm` / reasons, `tf` move, `calib` reply, `caps.move` |
| `firmware/esp32/src/net/motion_store.h/.cpp` (create) | NVS load / save of `counts_per_m` |
| `firmware/esp32/src/net/link_task.cpp` (modify) | `handleMove`, `handleCalib`, Uno events → motion, `MotionTelem` from the snapshot |
| `firmware/esp32/test/test_move/test_main.cpp` (create) | MoveController host tests with a plant |
| `firmware/esp32/test/test_motion/test_main.cpp` (modify) | router, service and link tests for move |
| `firmware/esp32/test/test_ws_codec/test_main.cpp`, `test/test_ws_json/test_main.cpp` (modify) | codec / JSON tests |
| `tools/smoke/move.py` (create), `tools/smoke/tests/test_move_math.py` (create), `tools/smoke/pyproject.toml` (modify) | the tool, its maths tests, pytest |

Branch first: `git checkout main && git pull --ff-only && git checkout -b plan-k`.

---

### Task 1: Routing and `caps.move` for an Uno with encoders

**Files:**
- Modify: `firmware/esp32/src/core/motion_router.cpp` (`routeMove`), `firmware/esp32/src/core/motion_router.h` (comment)
- Modify: `firmware/esp32/src/core/ws_json.cpp` (`jsonWelcome` caps.move)
- Test: `firmware/esp32/test/test_motion/test_main.cpp`, `firmware/esp32/test/test_ws_json/test_main.cpp`

**Interfaces:**
- Produces: `Route routeMove(uint8_t body_caps)` → `kForward` with `kMove`, `kLocal` with `kEncoders` (no `kMove`), else `kUnavailable`. `welcome.tank.caps.move` = `"true"` exactly when `routeMove` is Local.

- [ ] **Step 1: Write the failing tests**

In `test/test_motion/test_main.cpp`, after `test_router_full_body_forwards`, add (and `RUN_TEST(test_router_move_needs_encoders);` next to the other router RUN_TESTs):

```cpp
// Plan K (bridge LLD R7.2): an Uno with encoders gets MOVE from the bridge's motion service.
void test_router_move_needs_encoders() {
  const uint8_t enc = kLiteCaps | bit(proto::CapBit::kEncoders);
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeMove(enc)));
  TEST_ASSERT_EQUAL(R(Route::kUnavailable), R(routeMove(kLiteCaps)));  // encoders off -> no MOVE
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeMove(enc | bit(proto::CapBit::kMove))));
}
```

In `test/test_ws_json/test_main.cpp`, after `test_welcome_caps_bits`, add (and its `RUN_TEST`):

```cpp
// Plan K: caps.move is true when the bridge runs MOVE (Uno with encoders, no MOVE of its own).
void test_welcome_caps_move_with_encoders() {
  HelloInfo h;
  h.seen = true;
  h.fw = 0x0100;
  h.caps = (1u << static_cast<uint8_t>(proto::CapBit::kAudioLed)) | (1u << static_cast<uint8_t>(proto::CapBit::kEncoders));
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":true"));
  h.caps = 1u << static_cast<uint8_t>(proto::CapBit::kAudioLed);
  jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &h, "OV3660", kNone, "phone-1"});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":false"));
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion -f test_ws_json`
Expected: FAIL — `test_router_move_needs_encoders` (Unavailable ≠ Local) and `test_welcome_caps_move_with_encoders` (`"move":true` not found). Also `test_router_lite_runs_turn_and_rate_locally` still passes (kLiteCaps has no encoders).

- [ ] **Step 3: Implement**

`motion_router.cpp` — replace `routeMove`:

```cpp
Route routeMove(uint8_t body_caps) {
  if (has(body_caps, proto::CapBit::kMove)) return Route::kForward;
  return has(body_caps, proto::CapBit::kEncoders) ? Route::kLocal : Route::kUnavailable;  // Plan K: bridge MOVE
}
```

`motion_router.h` — change the `routeMove` declaration comment to: `Route routeMove(uint8_t body_caps);  // MOVE: body's own, else the bridge with the body's encoders (Plan K)`.

`ws_json.cpp` — next to `localOnly`, add:

```cpp
// Plan K (LLD R7.1): the bridge runs MOVE for a body with encoders but without its own MOVE.
const char* localMove(const HelloInfo& h) {
  return h.seen && routeMove(h.caps) == Route::kLocal ? "true" : "false";
}
```

and in `jsonWelcome` replace `capBit(*w.hello, proto::CapBit::kMove)` with `localMove(*w.hello)`. Add `#include "core/motion_router.h"` to `ws_json.cpp` if not already included. Update the comment above `localOnly` ("move still needs the body's encoders") to "move: run by the bridge when the body has encoders (Plan K)".

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: all suites PASS (159 + 2 new = 161 test cases).

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/motion_router.* firmware/esp32/src/core/ws_json.cpp firmware/esp32/test/test_motion/test_main.cpp firmware/esp32/test/test_ws_json/test_main.cpp
git commit -m "feat(bridge): route MOVE to the motion service for an Uno with encoders (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `MoveController` (distance, profile, ends)

**Files:**
- Modify: `firmware/esp32/src/core/motion_config.h`
- Create: `firmware/esp32/src/core/move_controller.h`, `firmware/esp32/src/core/move_controller.cpp`
- Test: `firmware/esp32/test/test_move/test_main.cpp` (new suite)

**Interfaces:**
- Produces (namespace `cq::core`):
  - `struct MotionTelem { uint16_t yaw_cdeg = 0; int16_t rate_ddps = 0; int32_t enc_l = 0; int32_t enc_r = 0; uint32_t flags = 0; };`
  - `struct MoveStep { int8_t v = 0; bool done = false; bool stalled = false; bool blocked = false; bool timeout = false; };`
  - `class MoveController { void begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, uint32_t now_ms); MoveStep step(const MotionTelem& t, uint32_t now_ms); uint8_t progressPct() const; int32_t movedMm() const; }` — `movedMm` is signed like `dist_cm`.
  - In `cq::core::motion`: `kMoveSlowCm`, `kMoveCrawlPct`, `kMoveSpeedMin`, `kMoveMaxCm`, `kMoveStallMs`, `kMoveStallCm`, `kMoveBlockedMs`, `kMoveTimeoutCmPerS`, `kMoveTimeoutBaseMs`, `kCpmDefault`, `kCpmMin`, `kCpmMax`, `bool validCpm(uint32_t n)`.

- [ ] **Step 1: Add the constants** (no behaviour yet) — append inside `namespace motion` in `motion_config.h`, before `// Service`:

```cpp
// MOVE by distance (Plan K, bridge LLD R7.3): counts from TELEM_FAST enc_l / enc_r (signed by drive direction).
constexpr int32_t kMoveSlowCm = 10;            // slow-down zone before the target
constexpr int32_t kMoveCrawlPct = 15;          // slowest v that still moves both tracks (floor-tuned)
constexpr uint8_t kMoveSpeedMin = 10;          // move speed 10..100 %
constexpr int32_t kMoveMaxCm = 500;            // |dist_cm| 1..500
constexpr uint32_t kMoveStallMs = 1500;        // stalled: < kMoveStallCm progress for 1.5 s
constexpr int32_t kMoveStallCm = 1;
constexpr uint32_t kMoveBlockedMs = 2000;      // blocked flag (obstacle brake) for 2 s -> end blocked
constexpr int32_t kMoveTimeoutCmPerS = 5;      // timeout = |cm| / 5 cm/s + 3 s
constexpr uint32_t kMoveTimeoutBaseMs = 3000;
// counts_per_m (Plan K, spec 02 §G4): NVS, default from the 160 cm tape run (design-notes §63).
constexpr uint16_t kCpmDefault = 7560;
constexpr uint16_t kCpmMin = 3000;
constexpr uint16_t kCpmMax = 15000;
inline bool validCpm(uint32_t n) { return n >= kCpmMin && n <= kCpmMax; }
```

- [ ] **Step 2: Write the failing tests** — create `firmware/esp32/test/test_move/test_main.cpp`:

```cpp
// Plan K (bridge LLD R7.3 / R7.8): MoveController against a straight-driving plant.
#include <unity.h>

#include "core/motion_config.h"
#include "core/move_controller.h"
#include "cq_protocol.h"

using namespace cq::core;
namespace proto = cq::proto;

namespace {
constexpr uint16_t kCpm = 7560;  // 75.6 counts / cm
const uint32_t kBlockedBit = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);

// Counts per 20 ms follow v with a 3-tick (60 ms) lag at 0.4 count per % per tick (40 % ~ 800 counts/s, both
// tracks); after the command drops to 0 the tank rolls on for 5 ticks at half its last step (~0.2 cm at a crawl).
struct Plant {
  int32_t enc_l = 1000, enc_r = 2000;  // arbitrary start counts
  uint32_t flags = 0;
  int8_t hist[3] = {0, 0, 0};
  uint8_t head = 0;
  int32_t last_step = 0;
  int roll_ticks = 0;
  void step(int8_t v) {
    const int8_t applied = hist[head];
    hist[head] = v;
    head = static_cast<uint8_t>((head + 1) % 3);
    int32_t d = applied * 4 / 10;
    if (applied == 0 && last_step != 0 && roll_ticks == 0) roll_ticks = 5;
    if (applied == 0 && roll_ticks > 0) {
      d = last_step / 2;
      if (--roll_ticks == 0) last_step = 0;
    } else {
      last_step = d;
    }
    enc_l += d;
    enc_r += d;
  }
  MotionTelem telem() const {
    MotionTelem t;
    t.enc_l = enc_l;
    t.enc_r = enc_r;
    t.flags = flags;
    return t;
  }
};

struct Run {
  MoveStep last;
  uint32_t t = 0;
  int8_t max_v = 0;
  int8_t min_abs_v = 127;
};

// Steps controller + plant every 20 ms until an end or `limit_ms`.
Run runUntilEnd(MoveController& mc, Plant& p, uint32_t limit_ms) {
  Run r;
  while (r.t < limit_ms) {
    r.t += 20;
    r.last = mc.step(p.telem(), r.t);
    if (r.last.done || r.last.stalled || r.last.blocked || r.last.timeout) break;
    const int8_t a = r.last.v < 0 ? static_cast<int8_t>(-r.last.v) : r.last.v;
    if (a > r.max_v) r.max_v = a;
    if (a < r.min_abs_v) r.min_abs_v = a;
    p.step(r.last.v);
  }
  return r;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_move_done_at_target() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_EQUAL_INT8(0, r.last.v);
  TEST_ASSERT_INT32_WITHIN(5, 1000, mc.movedMm());  // counted at the stop: at most one crawl tick past 100 cm
  TEST_ASSERT_GREATER_OR_EQUAL_INT32(1000, mc.movedMm());
  TEST_ASSERT_EQUAL_UINT8(100, mc.progressPct());
}

void test_move_profile_cruise_then_crawl() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  const int32_t target = 100 * kCpm / 100, slow = motion::kMoveSlowCm * kCpm / 100;
  const int32_t start = (p.enc_l + p.enc_r) / 2;
  int8_t prev = 127;
  uint32_t t = 0;
  for (;;) {
    t += 20;
    const MoveStep s = mc.step(p.telem(), t);
    if (s.done) break;
    const int32_t remaining = target - ((p.enc_l + p.enc_r) / 2 - start);
    if (remaining >= slow) {
      TEST_ASSERT_EQUAL_INT8(40, s.v);  // cruise
    } else {
      TEST_ASSERT_TRUE(s.v <= prev);    // only falls in the slow zone ...
      TEST_ASSERT_TRUE(s.v >= motion::kMoveCrawlPct);  // ... never below the crawl
    }
    prev = s.v;
    p.step(s.v);
  }
  TEST_ASSERT_TRUE(prev <= motion::kMoveCrawlPct + 1);  // arrives at a crawl
}

void test_move_reverse_counts_down() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, -50, 40, kCpm, 0);
  const MoveStep first = mc.step(p.telem(), 20);
  TEST_ASSERT_EQUAL_INT8(-40, first.v);
  p.step(first.v);
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_TRUE(p.enc_l < 1000);  // counted down
  TEST_ASSERT_INT32_WITHIN(5, -500, mc.movedMm());
}

void test_move_progress_rises() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  TEST_ASSERT_EQUAL_UINT8(0, mc.progressPct());
  uint8_t last = 0;
  uint32_t t = 0;
  for (int i = 0; i < 200; ++i) {
    const MoveStep s = mc.step(p.telem(), t += 20);
    TEST_ASSERT_TRUE(mc.progressPct() >= last);
    last = mc.progressPct();
    p.step(s.v);
  }
  TEST_ASSERT_TRUE(last > 10 && last < 100);
}

void test_move_stalled_without_progress() {
  Plant p;  // the plant is never stepped: no counts at all
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  MoveStep s;
  for (uint32_t t = 20; t < motion::kMoveStallMs; t += 20) {
    s = mc.step(p.telem(), t);
    TEST_ASSERT_FALSE(s.stalled);
  }
  s = mc.step(p.telem(), motion::kMoveStallMs);
  TEST_ASSERT_TRUE(s.stalled);
}

void test_move_blocked_after_two_seconds_not_stalled() {
  Plant p;
  p.flags = kBlockedBit;  // obstacle brake holds the tank: no counts
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  MoveStep s;
  for (uint32_t t = 20; t < motion::kMoveBlockedMs; t += 20) {
    s = mc.step(p.telem(), t);
    TEST_ASSERT_FALSE(s.stalled);
    TEST_ASSERT_FALSE(s.blocked);
  }
  s = mc.step(p.telem(), motion::kMoveBlockedMs + 20);
  TEST_ASSERT_TRUE(s.blocked);
}

void test_move_brief_block_continues() {  // Review Focus 2
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 30, 40, kCpm, 0);
  uint32_t t = 0;
  for (int i = 0; i < 50; ++i) p.step(mc.step(p.telem(), t += 20).v);  // 1 s driving
  p.flags = kBlockedBit;
  for (int i = 0; i < 50; ++i) TEST_ASSERT_FALSE(mc.step(p.telem(), t += 20).blocked);  // 1 s held, no counts
  p.flags = 0;
  MoveStep s;
  do {
    s = mc.step(p.telem(), t += 20);
    TEST_ASSERT_FALSE(s.stalled || s.blocked || s.timeout);
    p.step(s.v);
  } while (!s.done && t < 30000);
  TEST_ASSERT_TRUE(s.done);
}

void test_move_timeout() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  const uint32_t deadline = 100 * 1000 / motion::kMoveTimeoutCmPerS + motion::kMoveTimeoutBaseMs;  // 23 s
  MoveStep s;
  for (uint32_t t = 20; t < deadline; t += 20) {
    if (t % 1400 == 0) p.enc_l += 80, p.enc_r += 80;  // just over 1 cm every 1.4 s: never stalled, far too slow
    s = mc.step(p.telem(), t);
    TEST_ASSERT_FALSE(s.timeout || s.stalled || s.done);
  }
  s = mc.step(p.telem(), deadline);
  TEST_ASSERT_TRUE(s.timeout);
}

void test_move_counts_reset_ends_stalled() {  // Review Focus 1: the Uno restarted mid-move
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 200, 40, kCpm, 0);
  uint32_t t = 0;
  for (int i = 0; i < 100; ++i) p.step(mc.step(p.telem(), t += 20).v);  // 2 s driving
  p.enc_l = 0;
  p.enc_r = 0;
  const uint32_t reset_at = t;
  MoveStep s;
  do {
    s = mc.step(p.telem(), t += 20);
    TEST_ASSERT_FALSE(s.done);
    p.step(s.v);
  } while (!s.stalled && t < reset_at + 5000);
  TEST_ASSERT_TRUE(s.stalled);
  TEST_ASSERT_TRUE(t - reset_at <= motion::kMoveStallMs + 100);
}

void test_move_speed_below_crawl_never_zero() {  // Review Focus 3
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 10, motion::kMoveSpeedMin, kCpm, 0);  // 10 cm at 10 % ~ 4 s (timeout 5 s)
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_EQUAL_INT8(motion::kMoveSpeedMin, r.min_abs_v);
  TEST_ASSERT_EQUAL_INT8(motion::kMoveSpeedMin, r.max_v);
}

void test_move_short_move_stays_slow() {  // Review Focus 4
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 1, 100, kCpm, 0);
  const Run r = runUntilEnd(mc, p, 10000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_TRUE(r.max_v <= 23);  // 15 + 85 × 75 / 756 at the start
  TEST_ASSERT_TRUE(r.min_abs_v >= motion::kMoveCrawlPct);
}

void test_cpm_limits() {
  TEST_ASSERT_TRUE(motion::validCpm(7560));
  TEST_ASSERT_TRUE(motion::validCpm(3000));
  TEST_ASSERT_TRUE(motion::validCpm(15000));
  TEST_ASSERT_FALSE(motion::validCpm(2999));
  TEST_ASSERT_FALSE(motion::validCpm(15001));
  TEST_ASSERT_FALSE(motion::validCpm(0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_move_done_at_target);
  RUN_TEST(test_move_profile_cruise_then_crawl);
  RUN_TEST(test_move_reverse_counts_down);
  RUN_TEST(test_move_progress_rises);
  RUN_TEST(test_move_stalled_without_progress);
  RUN_TEST(test_move_blocked_after_two_seconds_not_stalled);
  RUN_TEST(test_move_brief_block_continues);
  RUN_TEST(test_move_timeout);
  RUN_TEST(test_move_counts_reset_ends_stalled);
  RUN_TEST(test_move_speed_below_crawl_never_zero);
  RUN_TEST(test_move_short_move_stays_slow);
  RUN_TEST(test_cpm_limits);
  return UNITY_END();
}
```

Check how the other suites declare `main` (`grep -n "int main" firmware/esp32/test/test_motion/test_main.cpp`) and copy that exact form if it differs.

- [ ] **Step 3: Run to verify it fails**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_move`
Expected: build FAIL — `core/move_controller.h: No such file or directory`.

- [ ] **Step 4: Implement** — create `firmware/esp32/src/core/move_controller.h`:

```cpp
// MOVE by distance on the bridge (Plan K, bridge LLD R7.3): straight (w = 0, the Uno holds the heading), mean
// encoder distance, cruise then a linear slow-down to a crawl over the last 10 cm, stall / blocked / timeout ends.
// Whole numbers only.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

// The TELEM_FAST fields the motion service uses (0.01°, 0.1 °/s, counts, FastFlagBit).
struct MotionTelem {
  uint16_t yaw_cdeg = 0;
  int16_t rate_ddps = 0;
  int32_t enc_l = 0;
  int32_t enc_r = 0;
  uint32_t flags = 0;
};

struct MoveStep {
  int8_t v = 0;  // % for DRIVE (w = 0)
  bool done = false;
  bool stalled = false;
  bool blocked = false;
  bool timeout = false;
};

class MoveController {
 public:
  void begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, uint32_t now_ms);
  MoveStep step(const MotionTelem& t, uint32_t now_ms);
  uint8_t progressPct() const;
  int32_t movedMm() const;  // signed like dist_cm

 private:
  int32_t m_start_l = 0;
  int32_t m_start_r = 0;
  int32_t m_target = 1;       // counts, > 0
  int32_t m_slow = 1;         // slow zone, counts
  int32_t m_stall_counts = 1;  // progress that counts as moving
  int32_t m_done = 0;         // travelled in the move direction, counts
  int32_t m_ref = 0;          // m_done at the last progress
  int8_t m_dir = 1;
  uint8_t m_speed = 40;
  uint16_t m_cpm = 7560;
  uint32_t m_ref_ms = 0;
  uint32_t m_deadline = 0;
  bool m_blocked = false;
  uint32_t m_blocked_since = 0;
};

}  // namespace core
}  // namespace cq
```

Create `firmware/esp32/src/core/move_controller.cpp`:

```cpp
#include "core/move_controller.h"

#include "core/motion_config.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

void MoveController::begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm,
                           uint32_t now_ms) {
  const int32_t cm = dist_cm < 0 ? -dist_cm : dist_cm;
  m_start_l = enc_l;
  m_start_r = enc_r;
  m_dir = dist_cm < 0 ? -1 : 1;
  m_cpm = cpm;
  m_target = cm * cpm / 100;
  if (m_target < 1) m_target = 1;
  m_slow = motion::kMoveSlowCm * cpm / 100;
  m_stall_counts = motion::kMoveStallCm * cpm / 100;
  m_speed = speed_pct;
  m_done = 0;
  m_ref = 0;
  m_ref_ms = now_ms;
  m_deadline = now_ms + static_cast<uint32_t>(cm) * 1000u / motion::kMoveTimeoutCmPerS + motion::kMoveTimeoutBaseMs;
  m_blocked = false;
}

MoveStep MoveController::step(const MotionTelem& t, uint32_t now_ms) {
  MoveStep s;
  m_done = m_dir * ((t.enc_l - m_start_l) + (t.enc_r - m_start_r)) / 2;
  if (m_done >= m_target) {
    s.done = true;
    return s;
  }
  const bool blocked = ((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) != 0;
  if (blocked) {
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
  if (m_done - m_ref >= m_stall_counts) {
    m_ref = m_done;
    m_ref_ms = now_ms;
  } else if (now_ms - m_ref_ms >= motion::kMoveStallMs) {
    s.stalled = true;
    return s;
  }
  if (static_cast<int32_t>(now_ms - m_deadline) >= 0) {
    s.timeout = true;
    return s;
  }
  const int32_t crawl = m_speed < motion::kMoveCrawlPct ? m_speed : motion::kMoveCrawlPct;
  const int32_t remaining = m_target - m_done;  // > 0 here
  int32_t mag = m_speed;
  if (remaining < m_slow) mag = crawl + (m_speed - crawl) * remaining / m_slow;
  s.v = static_cast<int8_t>(m_dir * mag);
  return s;
}

uint8_t MoveController::progressPct() const {
  if (m_done <= 0) return 0;
  if (m_done >= m_target) return 100;
  return static_cast<uint8_t>(m_done * 100 / m_target);
}

int32_t MoveController::movedMm() const { return m_dir * m_done * 1000 / m_cpm; }

}  // namespace core
}  // namespace cq
```

Note: the counts-reset case (Review Focus 1) gives a negative `m_done` and `remaining` > target, so v stays at `speed` until the stall check ends it (< 1.5 s) — `mag` never exceeds `m_speed`.

- [ ] **Step 5: Run to verify it passes**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_move`
Expected: 12 test cases PASS. If `test_move_done_at_target`'s `INT32_WITHIN(5, 1000, …)` misses because of the plant's 3-tick lag, read the actual `movedMm()` value: `done` is seen on the first frame past the target, so at the crawl (6 counts / tick) it overshoots by at most ~6 counts ≈ 0.8 mm and the bound `[1000, 1005]` must hold — a larger value is a controller bug (it did not slow down), not a test to loosen.

- [ ] **Step 6: Run the whole native suite and commit**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native` → all PASS.

```bash
git add firmware/esp32/src/core/motion_config.h firmware/esp32/src/core/move_controller.* firmware/esp32/test/test_move/test_main.cpp
git commit -m "feat(bridge): MoveController - encoder distance, slow-down to a crawl, stall / blocked / timeout (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Motion service and link run moves (incl. Uno stall / encoder-fault events)

**Files:**
- Modify: `firmware/esp32/src/core/motion_service.h`, `firmware/esp32/src/core/motion_service.cpp`
- Modify: `firmware/esp32/src/core/motion_link.h`, `firmware/esp32/src/core/motion_link.cpp`
- Test: `firmware/esp32/test/test_motion/test_main.cpp`

**Interfaces:**
- Consumes: `MotionTelem`, `MoveController`, `MoveStep` (Task 2).
- Produces:
  - `enum class PrimKind : uint8_t { kNone, kTurn, kRate, kMove };`
  - `EndReason` gains `kBlocked`, `kEncoderFault` **appended after `kDiverged`** (the JSON reason table is indexed by it).
  - `PrimitiveEnd` gains `bool has_moved = false; int32_t moved_mm = 0;`.
  - `void MotionService::startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, int32_t id, uint32_t now_ms);`
  - `MotionOut MotionService::onTelemetry(const MotionTelem& t, uint32_t now_ms);` — the old `onTelemetry(uint16_t yaw, int16_t rate, uint32_t now)` stays as a thin overload (turn / rate callers and tests).
  - `LinkOut MotionLink::onTelemetry(const MotionTelem& t, uint32_t now_ms);` (+ the same old overload).
  - `MotionEvent` gains `kBodyStall`, `kBodyEncoderFault`: they end a running **move** with `kStalled` / `kEncoderFault` and a STOP while the client owns the tank; for turn / rate they do nothing.

- [ ] **Step 1: Write the failing tests** — in `test/test_motion/test_main.cpp` add `#include "core/move_controller.h"` and, after the link tests, add (and the RUN_TESTs before `return UNITY_END();`):

```cpp
// ---------- Plan K (bridge LLD R7.4 / R7.6): moves in the service and the link ----------
namespace {
MotionTelem encAt(int32_t counts) {
  MotionTelem t;
  t.enc_l = counts;
  t.enc_r = counts;
  return t;
}
struct MoveRig {
  LinkOwner owner;
  MotionLink ml{owner};
  MoveRig() {
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);  // the move command took ownership
    ml.service().startMove(kFd, encAt(0), 10, 40, motion::kCpmDefault, 4, 0);  // 10 cm = 756 counts
  }
};
}  // namespace

void test_service_move_runs_and_ends_done_with_moved() {
  MotionService svc;
  svc.startMove(3, encAt(500), 10, 40, motion::kCpmDefault, 5, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(svc.kind()));
  MotionOut o;
  int32_t counts = 500;
  uint32_t t = 0;
  for (int i = 0; i < 1000 && !o.ended; ++i) {
    o = svc.onTelemetry(encAt(counts), t += 20);
    if (!o.ended) {
      TEST_ASSERT_TRUE(o.send);
      TEST_ASSERT_EQUAL_INT8(0, o.w);
      TEST_ASSERT_TRUE(o.v > 0);
    }
    counts += 20;
  }
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_TRUE(o.end.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(o.end.kind));
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(o.end.reason));
  TEST_ASSERT_TRUE(o.end.has_moved);
  TEST_ASSERT_TRUE(o.end.moved_mm >= 100 && o.end.moved_mm <= 103);
  TEST_ASSERT_EQUAL_INT32(5, o.end.id);
  TEST_ASSERT_FALSE(svc.active());
}

void test_service_move_progress_and_blocked_end() {
  MotionService svc;
  svc.startMove(3, encAt(0), 10, 40, motion::kCpmDefault, -1, 0);
  svc.onTelemetry(encAt(378), 20);  // half way
  TEST_ASSERT_EQUAL_UINT8(50, svc.progressPct());
  MotionTelem held = encAt(378);
  held.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);
  MotionOut o;
  for (uint32_t t = 40; t <= 2100 && !o.ended; t += 20) o = svc.onTelemetry(held, t);
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kBlocked), static_cast<int>(o.end.reason));
  TEST_ASSERT_TRUE(o.end.has_moved);
}

void test_turn_end_has_no_moved() {
  MotionService svc;
  svc.startTurn(3, 0, 9000, 40, 1, 0);
  const MotionOut o = svc.cancel(EndReason::kCancelled);
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_FALSE(o.end.has_moved);
}

void test_link_move_sends_auto_drive_straight() {
  MoveRig r;
  const LinkOut o = r.ml.onTelemetry(encAt(0), 20);
  TEST_ASSERT_TRUE(o.drive);
  TEST_ASSERT_EQUAL_INT8(40, o.v);
  TEST_ASSERT_EQUAL_INT8(0, o.w);
}

void test_link_uno_stall_and_fault_end_a_move_with_stop() {
  const struct { MotionEvent e; EndReason why; } cases[] = {{MotionEvent::kBodyStall, EndReason::kStalled},
                                                            {MotionEvent::kBodyEncoderFault, EndReason::kEncoderFault}};
  for (const auto& c : cases) {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.onEvent(c.e, 30);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_TRUE(o.stop);  // still the owner: the move ends cleanly with a STOP
    TEST_ASSERT_FALSE(o.drive);
    TEST_ASSERT_EQUAL(static_cast<int>(c.why), static_cast<int>(o.end.reason));
    TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(o.end.kind));
    TEST_ASSERT_FALSE(r.ml.service().active());
  }
}

void test_link_uno_stall_leaves_a_turn_alone() {
  Rig r;  // the Plan H turn rig
  r.ml.onTelemetry(0, 0, 20);
  TEST_ASSERT_FALSE(r.ml.onEvent(MotionEvent::kBodyStall, 30).event);
  TEST_ASSERT_FALSE(r.ml.onEvent(MotionEvent::kBodyEncoderFault, 40).event);
  TEST_ASSERT_TRUE(r.ml.service().active());
}

void test_link_move_ends_like_turn() {  // cancel / STOP / owner loss / gap - the Plan H rules for a move
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.onEvent(MotionEvent::kCancel, 30);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kCancelled), static_cast<int>(o.end.reason));
  }
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.onEvent(MotionEvent::kStop, 30);
    TEST_ASSERT_FALSE(o.stop);  // the STOP is already on its way
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStop), static_cast<int>(o.end.reason));
  }
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    r.owner.onUnoOwner(proto::Owner::kIr);
    const LinkOut o = r.ml.onTelemetry(encAt(20), 40);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_FALSE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kNotOwner), static_cast<int>(o.end.reason));
  }
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.tick(300);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kLink), static_cast<int>(o.end.reason));
  }
}
```

RUN_TESTs to add:

```cpp
  RUN_TEST(test_service_move_runs_and_ends_done_with_moved);
  RUN_TEST(test_service_move_progress_and_blocked_end);
  RUN_TEST(test_turn_end_has_no_moved);
  RUN_TEST(test_link_move_sends_auto_drive_straight);
  RUN_TEST(test_link_uno_stall_and_fault_end_a_move_with_stop);
  RUN_TEST(test_link_uno_stall_leaves_a_turn_alone);
  RUN_TEST(test_link_move_ends_like_turn);
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion`
Expected: build FAIL — `startMove` / `PrimKind::kMove` / `MotionEvent::kBodyStall` not declared.

- [ ] **Step 3: Implement the service** — `motion_service.h`:
  - `#include "core/move_controller.h"`.
  - `enum class PrimKind : uint8_t { kNone, kTurn, kRate, kMove };`
  - In `EndReason` after `kDiverged,` add:
    ```cpp
      kBlocked,       // Plan K: the obstacle brake held a move for 2 s
      kEncoderFault,  // Plan K: the Uno reported an encoder fault during a move (distance not trusted)
    ```
  - In `PrimitiveEnd` add:
    ```cpp
      bool has_moved = false;  // Plan K: a move reports its counted distance
      int32_t moved_mm = 0;
    ```
  - Public: 
    ```cpp
      void startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, int32_t id,
                     uint32_t now_ms);
      MotionOut onTelemetry(const MotionTelem& t, uint32_t now_ms);
      MotionOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {  // turn / rate callers
        MotionTelem t;
        t.yaw_cdeg = yaw_cdeg;
        t.rate_ddps = rate_ddps;
        return onTelemetry(t, now_ms);
      }
    ```
    (replace the old `onTelemetry` declaration).
  - Private: `MotionOut stepMove(const MotionTelem& t, uint32_t now_ms);` and member `MoveController m_move;`.
  - Update the file's top comment: "runs one TURN, DRIVE_RATE or MOVE (Plan K) …".

  `motion_service.cpp`:

```cpp
void MotionService::startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm,
                              int32_t id, uint32_t now_ms) {
  m_kind = PrimKind::kMove;
  m_fd = fd;
  m_id = id;
  m_move.begin(t.enc_l, t.enc_r, dist_cm, speed_pct, cpm, now_ms);
  m_last_telem_ms = now_ms;
}

MotionOut MotionService::stepMove(const MotionTelem& t, uint32_t now_ms) {
  const MoveStep s = m_move.step(t, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.blocked) return finish(EndReason::kBlocked);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.v = s.v;  // w = 0: the Uno's heading hold keeps it straight
  return o;
}
```

  In `finish`, before `m_kind = PrimKind::kNone;` add:

```cpp
  if (m_kind == PrimKind::kMove) {
    o.end.has_moved = true;
    o.end.moved_mm = m_move.movedMm();
  }
```

  Replace `onTelemetry` and `progressPct`:

```cpp
MotionOut MotionService::onTelemetry(const MotionTelem& t, uint32_t now_ms) {
  if (!active()) return MotionOut{};
  MotionOut o;
  switch (m_kind) {
    case PrimKind::kTurn: o = stepTurn(t.yaw_cdeg, t.rate_ddps, now_ms); break;
    case PrimKind::kMove: o = stepMove(t, now_ms); break;
    default: o = stepRate(t.rate_ddps, now_ms); break;
  }
  m_last_telem_ms = now_ms;
  return o;
}

uint8_t MotionService::progressPct() const {
  if (m_kind == PrimKind::kTurn) return m_turn.progressPct();
  if (m_kind == PrimKind::kMove) return m_move.progressPct();
  return 0;
}
```

- [ ] **Step 4: Implement the link** — `motion_link.h`: add to `MotionEvent` after `kTxFull`:

```cpp
  kBodyStall,         // Plan K: Uno EVENT stall - ends a move (stalled) with a STOP; turn / rate unchanged
  kBodyEncoderFault,  // Plan K: Uno EVENT encoder_fault - ends a move (encoder_fault) with a STOP
```

  and replace the `onTelemetry` declaration with:

```cpp
  LinkOut onTelemetry(const MotionTelem& t, uint32_t now_ms);
  LinkOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {  // turn / rate callers
    MotionTelem t;
    t.yaw_cdeg = yaw_cdeg;
    t.rate_ddps = rate_ddps;
    return onTelemetry(t, now_ms);
  }
```

  `motion_link.cpp`: change `MotionLink::onTelemetry` to take `const MotionTelem& t` and call `m_svc.onTelemetry(t, now_ms)` (body otherwise unchanged). In `onEvent`, right after `const int fd = m_svc.ownerFd();` add:

```cpp
  if (e == MotionEvent::kBodyStall || e == MotionEvent::kBodyEncoderFault) {
    if (m_svc.kind() != PrimKind::kMove) return LinkOut{};  // turn / rate keep their own stall checks
    const EndReason r = e == MotionEvent::kBodyStall ? EndReason::kStalled : EndReason::kEncoderFault;
    return finished(m_svc.cancel(r), fd, now_ms);  // the Uno already stopped both tracks; STOP while owner
  }
```

- [ ] **Step 5: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: all PASS (Plan H turn / rate / link tests unchanged and green, 7 new move tests green). `ws_json.cpp`'s `primName` still prints "turn" for kMove — fixed in Task 5; the build stays clean because it is a ternary, not a switch.

- [ ] **Step 6: Commit**

```bash
git add firmware/esp32/src/core/motion_service.* firmware/esp32/src/core/motion_link.* firmware/esp32/test/test_motion/test_main.cpp
git commit -m "feat(bridge): motion service runs MOVE; Uno stall / encoder fault end a move with a STOP (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: WebSocket parsing of `move` and `calib`

**Files:**
- Modify: `firmware/esp32/src/core/ws_codec.h`, `firmware/esp32/src/core/ws_codec.cpp`
- Test: `firmware/esp32/test/test_ws_codec/test_main.cpp`

**Interfaces:**
- Consumes: `motion::kMoveSpeedMin`, `motion::kMoveMaxCm`, `motion::kCpmMin/kCpmMax`, `motion::kTurnWMin`.
- Produces: `WsCmdKind::kMove`, `WsCmdKind::kCalib` (inserted before `kUnsupported`); `WsCommand` fields `int16_t move_cm = 0;`, `bool has_cpm = false;`, `uint16_t cpm = 0;` (`speed` reused, default 40).

- [ ] **Step 1: Write the failing tests** — add to `test/test_ws_codec/test_main.cpp` (and their RUN_TESTs):

```cpp
// Plan K (bridge LLD R7.1): move and calib.
void test_move_fields_and_ranges() {
  WsCommand c = P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":7}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(c.kind));
  TEST_ASSERT_EQUAL_INT16(100, c.move_cm);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);
  TEST_ASSERT_EQUAL_INT32(7, c.id);
  c = P("{\"t\":\"move\",\"dist_cm\":-50}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(c.kind));
  TEST_ASSERT_EQUAL_INT16(-50, c.move_cm);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);  // default
  TEST_ASSERT_EQUAL_INT16(500, P("{\"t\":\"move\",\"dist_cm\":500}").move_cm);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(P("{\"t\":\"move\",\"dist_cm\":1,\"speed\":100}").kind));
  const char* bad[] = {
      "{\"t\":\"move\"}",                                  // no distance
      "{\"t\":\"move\",\"dist_cm\":0}",                    // zero
      "{\"t\":\"move\",\"dist_cm\":501}",                  // too far
      "{\"t\":\"move\",\"dist_cm\":-501}",
      "{\"t\":\"move\",\"dist_cm\":10.5}",                 // whole cm only
      "{\"t\":\"move\",\"dist_cm\":10,\"speed\":9}",       // below 10 %
      "{\"t\":\"move\",\"dist_cm\":10,\"speed\":101}",
      "{\"t\":\"move\",\"dist_cm\":10,\"speed\":40.5}",
      "{\"t\":\"move\",\"dist_cm\":10,\"heading\":90}",    // not supported in Plan K
  };
  for (const char* s : bad) TEST_ASSERT_EQUAL_MESSAGE(K(WsCmdKind::kInvalid), K(P(s).kind), s);
}

void test_speed_limits_per_command() {  // Review Focus 5
  TEST_ASSERT_EQUAL(K(WsCmdKind::kMove), K(P("{\"t\":\"move\",\"dist_cm\":10,\"speed\":10}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":10}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":12}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(P("{\"speed\":12,\"by\":90,\"t\":\"turn\"}").kind));  // any key order
}

void test_calib_read_and_set() {
  WsCommand c = P("{\"t\":\"calib\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(c.kind));
  TEST_ASSERT_FALSE(c.has_cpm);
  c = P("{\"t\":\"calib\",\"counts_per_m\":7480,\"id\":3}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(c.kind));
  TEST_ASSERT_TRUE(c.has_cpm);
  TEST_ASSERT_EQUAL_UINT16(7480, c.cpm);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"counts_per_m\":3000}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"counts_per_m\":15000}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":2999}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":15001}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":7480.5}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"counts_per_m\":\"x\"}").kind));
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec`
Expected: build FAIL — `kMove` / `move_cm` / `kCalib` not members.

- [ ] **Step 3: Implement** — `ws_codec.h`: before `kUnsupported,` add

```cpp
  kMove,       // Plan K (LLD R7.1): {"t":"move","dist_cm":±1..500,"speed"?:10..100} -> motion service
  kCalib,      // {"t":"calib","counts_per_m"?:3000..15000} -> read / set the bridge's counts_per_m (NVS)
```

  and in `WsCommand` after `uint8_t debug_hz = 10;` add

```cpp
  int16_t move_cm = 0;    // move: ±1..500 (+ forward)
  bool has_cpm = false;   // calib: set (else read)
  uint16_t cpm = 0;       // calib: counts per metre
```

  Change the `speed` comment to `// turn: 12..100 %, move: 10..100 % (checked per command)`.

  `ws_codec.cpp`:
  - `kindOf`: add `if (strcmp(t, "move") == 0) return WsCmdKind::kMove;` and `if (strcmp(t, "calib") == 0) return WsCmdKind::kCalib;` before `return WsCmdKind::kUnsupported;`.
  - `readMotionField`, the `speed` branch: the field may arrive before `t`, so check only 1…100 here:

```cpp
  } else if (speed) {  // 1..100 here; the per-command minimum (turn w_min, move 10 %) is checked in complete()
    mf.bad = n < 1.0 || n > 100.0 || !isWhole(n);
    if (!mf.bad) c.speed = static_cast<uint8_t>(n);
```

  - `Seen`: add `bool dist = false, heading = false;`.
  - `readField`, before `return r.skip();`:

```cpp
  if (strcmp(key, "dist_cm") == 0) {  // move: whole cm, 1..500 in size
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num == 0.0 || fabs(num) > motion::kMoveMaxCm) return false;
    c.move_cm = static_cast<int16_t>(num);
    s.dist = true;
    return true;
  }
  if (strcmp(key, "heading") == 0) {  // move heading: not in Plan K (spec 02 §G1) - refused in complete()
    s.heading = true;
    return r.skip();
  }
  if (strcmp(key, "counts_per_m") == 0) {  // calib set
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < motion::kCpmMin || num > motion::kCpmMax) return false;
    c.has_cpm = true;
    c.cpm = static_cast<uint16_t>(num);
    return true;
  }
```

  - `complete`: replace the `kTurn` case and add the new ones:

```cpp
    case WsCmdKind::kTurn: return s.mf.by != s.mf.to && c.speed >= motion::kTurnWMin;  // exactly one of by / to
    case WsCmdKind::kMove: return s.dist && !s.heading && c.speed >= motion::kMoveSpeedMin;
    case WsCmdKind::kCalib: return true;
```

  Check that `motion_config.h` is already included by `ws_codec.cpp` (it uses `motion::kTurnWMin`); `fabs` needs `<math.h>` (already used for `lround`).

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: all PASS, including the existing turn speed tests (a turn with speed below 12 is still invalid).

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/ws_codec.* firmware/esp32/test/test_ws_codec/test_main.cpp
git commit -m "feat(bridge): parse move (dist_cm, speed) and calib (counts_per_m) commands (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: JSON for move ends, progress and `calib`

**Files:**
- Modify: `firmware/esp32/src/core/ws_json.h`, `firmware/esp32/src/core/ws_json.cpp`
- Test: `firmware/esp32/test/test_ws_json/test_main.cpp`

**Interfaces:**
- Consumes: `PrimKind::kMove`, `EndReason::kBlocked / kEncoderFault`, `PrimitiveEnd.has_moved / moved_mm` (Task 3).
- Produces: `size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m);` → `{"t":"calib","counts_per_m":n}`; `primitive_done` with `"prim":"move"` and `,"moved_cm":x.x` before `id`; `tf.primitive.kind:"move"`.

- [ ] **Step 1: Write the failing tests** (and RUN_TESTs):

```cpp
// Plan K (bridge LLD R7.1): move ends, progress, calib reply.
void test_primitive_done_move_with_moved_cm() {
  PrimitiveEnd e;
  e.kind = PrimKind::kMove;
  e.ok = true;
  e.reason = EndReason::kDone;
  e.id = 7;
  e.has_moved = true;
  e.moved_mm = 996;
  jsonPrimitiveDone(buf, sizeof(buf), 3, 500, e);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"ev\",\"seq\":3,\"tank_ms\":500,\"kind\":\"primitive_done\",\"prim\":\"move\",\"ok\":true,"
      "\"reason\":\"done\",\"moved_cm\":99.6,\"id\":7}",
      buf);
  e.ok = false;
  e.id = -1;
  e.moved_mm = -503;
  e.reason = EndReason::kBlocked;
  jsonPrimitiveDone(buf, sizeof(buf), 4, 600, e);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"blocked\",\"moved_cm\":-50.3}"));
  e.reason = EndReason::kEncoderFault;
  jsonPrimitiveDone(buf, sizeof(buf), 5, 700, e);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"encoder_fault\""));
}

void test_tf_move_progress_and_calib_reply() {
  TankSnapshot s;
  s.fast_valid = true;
  jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kMove, 42});
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"move\",\"pct\":42}"));
  jsonCalib(buf, sizeof(buf), 7480);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"calib\",\"counts_per_m\":7480}", buf);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_json`
Expected: build FAIL — `jsonCalib` not declared.

- [ ] **Step 3: Implement** — `ws_json.h`: update the `jsonPrimitiveDone` comment to `"prim":"turn"|"rate"|"move", … ,"moved_cm"? (move), "id"?` and add

```cpp
// Plan K (LLD R7.1): {"t":"calib","counts_per_m":n} - reply to calib (read or after a save).
size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m);
```

  `ws_json.cpp`:
  - `primName`: `return k == PrimKind::kRate ? "rate" : k == PrimKind::kMove ? "move" : "turn";`
  - `jsonPrimitiveDone`: the reasons table becomes `{"done", "cancelled", "stop", "not_owner", "stalled", "timeout", "link", "deadman", "diverged", "blocked", "encoder_fault"}`; add

```cpp
  char moved[24] = "";
  if (e.has_moved) {
    const long mm = static_cast<long>(e.moved_mm);
    const long a = mm < 0 ? -mm : mm;
    snprintf(moved, sizeof(moved), ",\"moved_cm\":%s%ld.%ld", mm < 0 ? "-" : "", a / 10, a % 10);
  }
```

    and change the format tail to `"\"ok\":%s,\"reason\":\"%s\"%s%s}"` passing `moved, id` (moved before id).
  - Add:

```cpp
size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m) {
  return fmt(out, cap, "{\"t\":\"calib\",\"counts_per_m\":%u}", static_cast<unsigned>(counts_per_m));
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: all PASS (the Plan H `test_primitive_done_and_tf_progress` string is unchanged: no `moved_cm` for turns).

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/ws_json.* firmware/esp32/test/test_ws_json/test_main.cpp
git commit -m "feat(bridge): primitive_done move with moved_cm, blocked / encoder_fault reasons, calib reply (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: NVS store and link-task wiring (firmware build + stand check)

**Files:**
- Create: `firmware/esp32/src/net/motion_store.h`, `firmware/esp32/src/net/motion_store.cpp`
- Modify: `firmware/esp32/src/net/link_task.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1–5.
- Produces: `uint16_t cq::net::motionStoreLoad();` (default when missing / out of range), `bool cq::net::motionStoreSave(uint16_t cpm);`; WebSocket `move` / `calib` live on the bridge.

This task is glue on the ESP32 (Preferences, FreeRTOS link task): no host test can run it; its checks are the `bridge` build plus the stand check in Step 6.

- [ ] **Step 1: Create the store** — `net/motion_store.h`:

```cpp
// Plan K (spec 02 §G4, bridge LLD R7.2): counts_per_m in NVS (namespace "cq_motion", key "cpm").
#pragma once

#include <stdint.h>

namespace cq {
namespace net {

uint16_t motionStoreLoad();             // the saved value, or motion::kCpmDefault when missing / out of range
bool motionStoreSave(uint16_t cpm);     // false when out of range or the flash write failed

}  // namespace net
}  // namespace cq
```

  `net/motion_store.cpp`:

```cpp
#include "net/motion_store.h"

#include <Preferences.h>

#include "core/motion_config.h"
#include "net/log.h"

namespace cq {
namespace net {

uint16_t motionStoreLoad() {
  uint16_t v = core::motion::kCpmDefault;
  Preferences p;
  if (p.begin("cq_motion", true)) {  // read-only; fails when the namespace was never written -> default
    const uint16_t saved = p.getUShort("cpm", 0);
    if (core::motion::validCpm(saved)) v = saved;
    p.end();
  }
  logf("motion: counts_per_m %u", static_cast<unsigned>(v));
  return v;
}

bool motionStoreSave(uint16_t cpm) {
  if (!core::motion::validCpm(cpm)) return false;
  Preferences p;
  if (!p.begin("cq_motion", false)) return false;
  const bool ok = p.putUShort("cpm", cpm) == sizeof(uint16_t);
  p.end();
  if (ok) logf("motion: counts_per_m saved %u", static_cast<unsigned>(cpm));
  return ok;
}

}  // namespace net
}  // namespace cq
```

- [ ] **Step 2: Wire the link task** — in `net/link_task.cpp`:
  - `#include "net/motion_store.h"` and `#include "core/move_controller.h"` with the other includes.
  - Next to `g_motion`, add `uint16_t g_cpm = core::motion::kCpmDefault;  // Plan K: counts per metre (NVS)`.
  - Next to `motionOwnedBy`, add:

```cpp
// Plan K: the TELEM_FAST fields the motion service uses.
core::MotionTelem telemOf(const core::TankSnapshot& s) {
  core::MotionTelem t;
  t.yaw_cdeg = s.fast.yaw;
  t.rate_ddps = s.fast.yaw_rate;
  t.enc_l = s.fast.enc_l;
  t.enc_r = s.fast.enc_r;
  t.flags = s.fast.flags;
  return t;
}
```

  - `isMotion`: `return isStream(k) || k == core::WsCmdKind::kTurn || k == core::WsCmdKind::kMove;`
  - After `handleDriveRate`, add:

```cpp
// move (Plan K, LLD R7.6): Local for an Uno with encoders and no encoder fault; Forward / Unavailable answered
// UNAVAILABLE (no full-firmware body yet).
void handleMove(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (g_ota_active) {
    nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  if (core::routeMove(g_link.hello().caps) != core::Route::kLocal) {
    nack(fd, id, core::BridgeReason::kUnavailable);  // encoders off
    return;
  }
  const core::TankSnapshot& snap = g_link.snapshot();
  if (bodyOffline() || !snap.fast_valid) {
    nack(fd, id, core::BridgeReason::kBodyOffline);
    return;
  }
  if ((snap.fast.flags >> static_cast<unsigned>(proto::FastFlagBit::kEncoderFault)) & 1u) {
    nack(fd, id, core::BridgeReason::kUnavailable);  // an encoder is switched off until the Uno restarts
    return;
  }
  core::Actions a;
  const core::Decision d = g_owner.onDrive(fd, now, a);
  apply(a, now);
  if (d != core::Decision::kSend) {
    nack(fd, id, reasonOf(d));
    return;
  }
  endMotion(core::MotionEvent::kOwnerCommand, now);  // a new move replaces the old primitive
  g_motion.service().startMove(fd, telemOf(snap), c.move_cm, c.speed, g_cpm, id, now);
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

// calib (Plan K, LLD R7.1): read for anyone; set only by the driver, not while a primitive runs.
void handleCalib(int fd, int32_t id, const core::WsCommand& c) {
  if (c.has_cpm) {
    if (g_owner.driverFd() != fd) {
      nack(fd, id, core::BridgeReason::kNotOwner);
      return;
    }
    if (g_motion.service().active() || !net::motionStoreSave(c.cpm)) {
      nack(fd, id, core::BridgeReason::kBusy);
      return;
    }
    g_cpm = c.cpm;
    if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
  }
  sendTo(fd, core::jsonCalib(g_json, sizeof(g_json), g_cpm));
}
```

  - Dispatch switch: add `case core::WsCmdKind::kMove: handleMove(fd, id, c, now); break;` and `case core::WsCmdKind::kCalib: handleCalib(fd, id, c); break;`.
  - `handleLinkEvents`, `kUnoEvent` case, after the `broadcast(...)` of the Uno event:

```cpp
        if (ev.code == static_cast<uint8_t>(proto::EventKind::kStall)) {
          endMotion(core::MotionEvent::kBodyStall, now);  // Plan K: ends a move (turn / rate unchanged)
        } else if (ev.code == static_cast<uint8_t>(proto::EventKind::kEncoderFault)) {
          endMotion(core::MotionEvent::kBodyEncoderFault, now);
        }
```

  - `telemetry()`: `runMotion(g_motion.onTelemetry(telemOf(s), now), now);  // 50 Hz`.
  - `linkTaskStart`: before the task is created, add `g_cpm = net::motionStoreLoad();`.
  - Check `BridgeReason` has `kBusy`, `kNotOwner`, `kUnavailable`, `kBodyOffline` (`grep -n "enum class BridgeReason" -A20 src/core/ws_json.h`); all four are used by the existing handlers.

- [ ] **Step 3: Build**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge && ~/.platformio/penv/bin/pio test -e native`
Expected: `bridge SUCCESS`, all native tests PASS.

- [ ] **Step 4: Commit**

```bash
git add firmware/esp32/src/net/motion_store.* firmware/esp32/src/net/link_task.cpp
git commit -m "feat(bridge): move and calib commands live - NVS counts_per_m, Uno stall / fault end a move (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: OTA the bridge** (owner: tank on, on the stand, S1 run position). Password from `include/secrets.h` without printing it; filter it from the log; delete the log:

```bash
cd firmware/esp32 && L=$SCRATCH/ota.log; export CQ_OTA_PASSWORD="$(sed -nE 's/^#define CQ_OTA_PASSWORD "(.*)"/\1/p' include/secrets.h)"; ~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port <bridge IP> > $L 2>&1; echo rc=$?; grep -vF -- "$CQ_OTA_PASSWORD" $L | grep -E 'Result|ERROR|FAILED'; rm -f $L
curl -s -m 5 http://<bridge IP>/info | grep -o '"version":"[^"]*"'
```

Expected: `Result … OK`, version = the new commit.

- [ ] **Step 6: Stand check with a throwaway script** (the tool comes in Task 7): from `tools/smoke`, `uv run python -c` a WsClient session that sends `{"t":"calib"}` and prints the reply (expect `counts_per_m 7560`), then `{"t":"move","dist_cm":20,"speed":30,"id":1}` and prints the `ack` and the `primitive_done` (expect `prim move`, `ok true`, `reason done`, `moved_cm` ≈ 20.0) — the owner watches the tracks run and slow down before stopping. Then a second move of 100 cm and a `{"t":"stop"}` after 1 s: expect `reason stop`.

---

### Task 7: `move.py` tool (moves, calibration) with tested maths

**Files:**
- Create: `tools/smoke/move.py`, `tools/smoke/tests/test_move_math.py`
- Modify: `tools/smoke/pyproject.toml` (pytest dev dependency + `pythonpath`)

**Interfaces:**
- Consumes: `bridge_echo.WsClient` (`send(dict)`, `recv(timeout) -> dict | None`, `close()`); the bridge's `move`, `calib`, `debug`, `release_control`, `primitive_done`, `td` (`enc: [l, r]`, `yaw`).
- Produces: `counts_travelled(start: tuple[int, int], end: tuple[int, int], direction: int) -> int`, `new_cpm(counts: int, measured_cm: float) -> int`, `mean_cpm(runs: list[int]) -> int`.

- [ ] **Step 1: Add pytest** — `tools/smoke/pyproject.toml`: dev group becomes `dev = ["ruff>=0.7", "pyright>=1.1", "types-pyserial>=3.5", "pytest>=8"]`, and add

```toml
[tool.pytest.ini_options]
pythonpath = ["."]
testpaths = ["tests"]
```

Run: `cd tools/smoke && uv sync` → installs pytest.

- [ ] **Step 2: Write the failing tests** — `tools/smoke/tests/test_move_math.py`:

```python
"""Plan K: the maths of move.py (bridge LLD R7.7)."""

from __future__ import annotations

import pytest

from move import counts_travelled, mean_cpm, new_cpm


def test_counts_travelled_forward_and_reverse() -> None:
    assert counts_travelled((1000, 2000), (8560, 9560), 1) == 7560
    assert counts_travelled((1000, 2000), (-2780, -1780), -1) == 3780


def test_new_cpm_from_tape() -> None:
    assert new_cpm(7560, 100.0) == 7560
    assert new_cpm(7560, 98.5) == 7675  # went less far than counted -> more counts per metre
    assert new_cpm(7642, 101.2) == 7551


def test_new_cpm_rejects_bad_measure() -> None:
    with pytest.raises(ValueError):
        new_cpm(7560, 0.0)


def test_mean_cpm() -> None:
    assert mean_cpm([7675, 7660, 7690]) == 7675
    with pytest.raises(ValueError):
        mean_cpm([])
```

Run: `cd tools/smoke && uv run pytest -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'move'`.

- [ ] **Step 3: Write the tool** — `tools/smoke/move.py`:

```python
"""MOVE by distance over WiFi (spec 02 Part G, bridge LLD R7.7): plain moves and counts_per_m calibration.

    uv run python move.py 100 --speed 40      # forward 100 cm
    uv run python move.py -50                 # reverse 50 cm
    uv run python move.py --calibrate         # move 100 cm, type the tape distance, average, save

Takes control (the move command does), so release the phone page first. STOP from the phone or IR ends a move.
"""

from __future__ import annotations

import argparse
import contextlib
import time
from collections.abc import Callable
from typing import Any

from bridge_echo import WsClient

Message = dict[str, Any]


def counts_travelled(start: tuple[int, int], end: tuple[int, int], direction: int) -> int:
    """Mean of both tracks' counts in the move direction."""
    return direction * ((end[0] - start[0]) + (end[1] - start[1])) // 2


def new_cpm(counts: int, measured_cm: float) -> int:
    """Counts per metre from the counts travelled (roll-on included) and the tape distance."""
    if measured_cm <= 0:
        raise ValueError("measured distance must be > 0 cm")
    return round(counts * 100 / measured_cm)


def mean_cpm(runs: list[int]) -> int:
    if not runs:
        raise ValueError("no calibration runs")
    return round(sum(runs) / len(runs))


def wrap_deg(d: float) -> float:
    return (d + 540.0) % 360.0 - 180.0


class Session:
    """One WebSocket client: debug stream on (10 Hz) for the counts, pings so the bridge keeps it."""

    def __init__(self, host: str) -> None:
        self.ws = WsClient(host)
        self.ws.send({"t": "hello", "client": "move-tool", "proto": 1})
        self.ws.send({"t": "debug", "on": True, "hz": 10})
        self.last_td: Message | None = None
        self.last_ping = time.monotonic()
        self.next_id = 100

    def pump_until(self, want: Callable[[Message], bool], seconds: float) -> Message | None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            now = time.monotonic()
            if now - self.last_ping >= 1.0:  # the bridge closes a client silent for 3 s
                self.ws.send({"t": "ping", "t0": int(now * 1000)})
                self.last_ping = now
            msg = self.ws.recv(0.2)
            if msg is None:
                continue
            if msg.get("t") == "td":
                self.last_td = msg
            if want(msg):
                return msg
        return None

    def fresh_td(self) -> Message:
        self.last_td = None
        msg = self.pump_until(lambda m: m.get("t") == "td", 2.0)
        if msg is None:
            raise RuntimeError("no debug telemetry (td) from the bridge - is the Uno online?")
        return msg

    def counts_per_m(self) -> int:
        self.ws.send({"t": "calib"})
        msg = self.pump_until(lambda m: m.get("t") == "calib", 3.0)
        if msg is None:
            raise RuntimeError("no calib reply - bridge without Plan K firmware?")
        return int(msg["counts_per_m"])

    def save_counts_per_m(self, cpm: int) -> bool:
        self.next_id += 1
        mid = self.next_id
        self.ws.send({"t": "calib", "counts_per_m": cpm, "id": mid})
        msg = self.pump_until(lambda m: m.get("id") == mid and m.get("t") in ("ack", "nack"), 3.0)
        if msg is None or msg.get("t") == "nack":
            print(f"save refused: {msg}")
            return False
        return True

    def move(self, cm: int, speed: int) -> tuple[Message, int, float]:
        """Runs one move; returns (primitive_done, counts travelled after the roll-on, heading change)."""
        start = self.fresh_td()
        self.next_id += 1
        mid = self.next_id
        self.ws.send({"t": "move", "dist_cm": cm, "speed": speed, "id": mid})
        reply = self.pump_until(lambda m: m.get("id") == mid and m.get("t") in ("ack", "nack"), 3.0)
        if reply is None or reply.get("t") == "nack":
            raise RuntimeError(f"move refused: {reply}")
        done = self.pump_until(
            lambda m: m.get("kind") == "primitive_done" and m.get("prim") == "move", abs(cm) / 5 + 10
        )
        if done is None:
            raise RuntimeError("no primitive_done - press STOP")
        self.pump_until(lambda _m: False, 0.5)  # roll-on
        end = self.fresh_td()
        direction = 1 if cm > 0 else -1
        counts = counts_travelled(
            (int(start["enc"][0]), int(start["enc"][1])), (int(end["enc"][0]), int(end["enc"][1])), direction
        )
        return done, counts, wrap_deg(float(end["yaw"]) - float(start["yaw"]))

    def close(self) -> None:
        with contextlib.suppress(OSError):
            self.ws.send({"t": "debug", "on": False})
            self.ws.send({"t": "release_control"})
        self.ws.close()


def report(done: Message, counts: int, heading: float, cpm: int) -> None:
    after = counts * 100 / cpm
    print(
        f"{done.get('reason')}: counted {done.get('moved_cm')} cm at the stop, {after:.1f} cm after the "
        f"roll-on, heading {heading:+.1f} deg (counts {counts}, counts_per_m {cpm})"
    )


def calibrate(s: Session, speed: int) -> int:
    cpm = s.counts_per_m()
    runs: list[int] = []
    while True:
        input("Place the tank at the start of the 1 m tape, then press Enter to drive 100 cm ...")
        done, counts, heading = s.move(100, speed)
        report(done, counts, heading, cpm)
        text = input("How far did it go on the tape (cm)? (blank = discard this run) ").strip()
        if text:
            runs.append(new_cpm(counts, float(text)))
            print(f"run {len(runs)}: {runs[-1]} counts/m - mean of {len(runs)}: {mean_cpm(runs)}")
        choice = input("[a]gain, [s]ave the mean, [q]uit without saving? ").strip().lower()
        if choice == "s" and runs:
            mean = mean_cpm(runs)
            if s.save_counts_per_m(mean):
                print(f"saved counts_per_m {mean} (was {cpm})")
                return 0
            return 1
        if choice == "q":
            return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="MOVE by distance / counts_per_m calibration (Plan K)")
    ap.add_argument("cm", type=int, nargs="?", help="distance, + forward, - reverse (1..500)")
    ap.add_argument("--speed", type=int, default=40)
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--host", default="conqueror-bridge.local")
    args = ap.parse_args()
    if not args.calibrate and args.cm is None:
        ap.error("give a distance in cm, or --calibrate")
    s = Session(args.host)
    try:
        if args.calibrate:
            return calibrate(s, args.speed)
        done, counts, heading = s.move(args.cm, args.speed)
        report(done, counts, heading, s.counts_per_m())
        return 0 if done.get("ok") else 1
    finally:
        s.close()


if __name__ == "__main__":
    raise SystemExit(main())
```

Check the real `WsClient` signatures in `bridge_echo.py` (`grep -n "def " bridge_echo.py`) — `send(obj)`, `recv(timeout)`, `close()` — and adapt calls if they differ.

- [ ] **Step 4: Run the checks**

Run: `cd tools/smoke && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright move.py tests/test_move_math.py`
Expected: 4 passed; ruff clean; pyright `0 errors`. Fix any pyright strict complaint in the tool (e.g. `Any` from JSON is cast with `int(...)` / `float(...)` as written).

- [ ] **Step 5: Commit**

```bash
git add tools/smoke/move.py tools/smoke/tests/test_move_math.py tools/smoke/pyproject.toml tools/smoke/uv.lock
git commit -m "feat(tools): move.py - MOVE by distance and counts_per_m calibration over WiFi (plan K)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Floor acceptance and docs (owner present)

**Files:**
- Modify: `docs/design-notes.md` (new §69 results), `docs/lld/esp32-bridge-lld.md` (R7 "as built" notes), `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` (Part G acceptance "as run"), `CLAUDE.md` (plan status), `firmware/esp32/src/core/motion_config.h` (only if the crawl needs tuning)

- [ ] **Step 1: Stand check** (tracks off the ground): `uv run python move.py 50` → tracks run, slow down, stop; prints `done`. Run `move.py 100` and press STOP on the phone after ~1 s → `stop`. Also save-and-restart check: `uv run python -c` a session that sets `calib 7600`, then restart the tank, read `calib` → 7600, then set back 7560.

- [ ] **Step 2: Crawl and slow-speed check** (floor): `move.py 15 --speed 15` — both tracks must move from the start; `move.py 30 --speed 10` must end `done`, not `timeout` (the timeout assumes ≥ 5 cm/s). If a track does not move at 15 % (stalled / curving), raise `kMoveCrawlPct` in steps of 2 (rebuild, OTA, retest), record the value; commit `fix(bridge): MOVE crawl <n> % from the floor test (plan K)`.

- [ ] **Step 3: Calibration** (floor, taped 1 m): `uv run python move.py --calibrate` ×3 runs, save the mean. Record the three values and the mean.

- [ ] **Step 4: Accuracy runs** — record each with `td_record.py` in a second terminal when useful:
  - 3 × `move.py 100` (40 %), 1 × `move.py 200` (40 %), 2 × `move.py -50`, 2 × `move.py 100 --speed 70`.
  - The owner measures distance and sideways drift for each.
  - **Pass: ±1 cm at 40 %, ±2 cm at 70 %, sideways ~1 cm / 2.5 m.**

- [ ] **Step 5: End checks:** hold a track during `move.py 100` → `stalled`; a box ~40 cm ahead of `move.py 100` → the brake stops the tank, `blocked` after 2 s.

- [ ] **Step 6: Record and commit** — design-notes §69 (all numbers, crawl value, saved counts_per_m, pass / miss); spec Part G "as run" line; LLD R7 as-built notes (any constant changed); CLAUDE.md: Plan K line under the plan order and the plans table. If the stop misses ±1 cm, write it as an owner decision point (early stop by a learned roll-on — not done here).

```bash
git add docs CLAUDE.md firmware/esp32/src/core/motion_config.h
git commit -m "docs: design-notes §69 - Plan K floor results; spec Part G / LLD R7 as built

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push -u origin plan-k
```

- [ ] **Step 7: Final review and merge** — whole-branch review (`git diff main...plan-k`), fix Critical / Important findings with a failing test first, then **ask the owner before merging `plan-k` into main**.

---

### Task 9 (added 2026-10-17, owner): learned early stop (spec 02 §G6, LLD R7.9)

Floor result that triggered it: the count stop is accurate (100.0–100.3 cm counted) but the tracks roll on 2.4–2.8 cm after the STOP at the crawl (15 % → PWM ~116 after the Uno's dead-band lift, ~19 cm/s), so `move 100` ends at ~103 cm.

**Files:** Create `firmware/esp32/src/core/roll_learner.h/.cpp`; modify `motion_config.h` (`kRollDefaultMm 25`, `kRollMaxMm 80`, `kRollSettleMs 100`, `kRollWindowMs 600`), `move_controller.h/.cpp` (`begin(..., roll_mm = 0)`, `m_stop_at = max(target − roll counts, target / 2)`, slow-down toward `m_stop_at`), `motion_service.h/.cpp` (`startMove(..., roll_mm = 0)`), `ws_json.h/.cpp` (`jsonCalib(out, cap, cpm, roll_mm)`), `net/motion_store.h/.cpp` (`motionStoreLoadRoll`, `motionStoreSaveRoll`, key `roll`), `net/link_task.cpp` (learner after a move `done`, observe on each TELEM_FAST with `driven = pwm_l ≠ 0 ∨ pwm_r ≠ 0`, average saved when changed, `startMove(..., g_roll_mm)`, cancel on a new move). Tests: `test/test_move` (+5), `test/test_ws_json` (calib string).

- [x] **Step 1: failing tests** — `test_move_stops_early_by_roll` (stops 189 counts = 25 mm short, progress 97 %), `test_move_early_stop_never_before_half` (3 cm move stops ≥ 113 counts), `test_roll_learner_settles_and_measures` (143 counts → 18 mm after 100 ms unchanged), `test_roll_learner_gives_up_and_discards` (600 ms limit → 19 mm; driven → none; > 80 mm → none), `test_roll_average` ((3 × old + r + 2) / 4); calib JSON `{"t":"calib","counts_per_m":7480,"roll_mm":25}`. Ran: build FAIL (`roll_learner.h` missing, `jsonCalib` arity).
- [x] **Step 2: implement** as listed above (code in the commit).
- [x] **Step 3:** `pio test -e native` → 190/190; `pio run -e bridge` → SUCCESS.
- [ ] **Step 4: floor** — OTA; 3 × `move.py 100` (back edge on the start mark, read the tape at the back edge): after one or two moves **100 ± 1 cm**; the bridge log / `calib` shows the learned `roll_mm`.
