# Plan J — Straight-driving tuning (track balance + heading-hold integral) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive straight to ≤ 1 cm sideways over ~3.1 m by cancelling the tank's measured right bias (track balance) and removing the heading hold's steady error (integral term).

**Architecture:** A new setting `track_balance` (‰) feeds a feed-forward `balanceTracks()` and the encoder sync's count-ratio target; `HeadingHoldLite` becomes P + I + D with a clamped integral that resets whenever the hold restarts. A report tool turns a recorded run into the suggested balance. Uno Lite only for the behaviour; the full firmware only gets the settings slot.

**Tech Stack:** C++ Arduino AVR (`-Os -flto`, no heap / virtual / floats in `domain/lite`), Unity host tests, protogen, Python 3.12 + uv (ruff, pyright strict).

**Spec:** `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part F §F5; LLD `docs/lld/uno-lite-lld.md` §20; measurements design-notes §63–§64.

## Global Constraints
- Uno: no heap, no `virtual`, no `delay()`; domain code never includes `ports/` or `Arduino.h`; only `SafetySupervisor` makes motor output; `domain/lite` whole numbers only.
- Generated files are never edited by hand (`cd protocol && uv run protogen write`).
- Safety features never weakened; the Plan I stall / runaway latch and the 30 % threshold stay as they are.
- Uno Lite budget: flash ≤ 32 256 B (now 30 288 B), static RAM ≤ 1 560 B (now 1 524 B).
- `track_balance`: setting id **66**, global, `i16`, unit ‰, min −100, max 100, default 0; positive = the right track is asked for more.
- Heading-hold integral: `Ki = Kp / 4 per second` (`kIDivTicks = 400` control ticks of 10 ms), integral contribution clamped to ±15 %, reset on steering, stop, capture.
- Python: 3.12 via uv, type hints, ruff + pyright strict clean.
- Commits: Conventional Commits ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. **A saved Lite settings block from before this plan must still load** (encoders_enabled stays on) — Task 2 `test_settings_previous_block_still_loads`.
2. **The integral must not wind up while steering or stopped** and must reset on a new capture — Task 4 `test_hold_integral_resets_on_steer_and_stop`.
3. **Balance must never act while the driver steers, in reverse it must still favour the right track's magnitude, and never exceed ±100** — Task 3 tests.
4. **`track_balance = 0` must reproduce today's behaviour exactly** (all Plan I tests unchanged) — Task 3/4 run the full suite.
5. **The integral plus the Plan I runaway guard:** a growing integral must not trip the runaway guard in normal driving — Task 4 `test_hold_integral_settles_without_saturation`.

---

### Task 1: Setting `track_balance` (protocol + full firmware slot)

**Files:**
- Modify: `protocol/defs/settings.yaml` (append id 66)
- Regenerate: `cd protocol && uv run protogen write`
- Modify: `firmware/uno/src/domain/settings/settings.h` (`GlobalSettings` + `int16_t track_balance;` at the end), `settings.cpp` (descriptor slot `G(track_balance, kTrackBalance)`), `settings_saver.h` (`kEeLayoutVersion = 2`)

**Interfaces:** Produces `proto::SettingId::kTrackBalance` (66).

- [ ] **Step 1:** append to `protocol/defs/settings.yaml` after `batt_hyst`:
```yaml
- { id: 66, name: track_balance, scope: global, type: i16, unit: permille, min: -100, max: 100, default: 0, doc: "straight driving: the right track is asked for this many per mille more than the left (spec 02 §F5.1)" }
```
Run: `cd protocol && uv run protogen write && uv run protogen check && uv run pytest -q` — Expected: files written, check 0, tests pass.
- [ ] **Step 2: Run the full-firmware settings tests to see them fail** — `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_settings` — Expected: `test_every_descriptor_has_a_slot` FAILS (id 66 has no slot).
- [ ] **Step 3:** in `settings.h` add `int16_t track_balance;     // ‰, spec 02 §F5.1 (Lite only uses it)` as the last `GlobalSettings` field and update the size comment; in `settings.cpp` add `G(track_balance, kTrackBalance),` after `G(batt_hyst, kBattHyst)`; in `settings_saver.h` set `kEeLayoutVersion = 2` (the block size changed; a full board's EEPROM resets to defaults once).
- [ ] **Step 4:** `~/.platformio/penv/bin/pio test -e native && ~/.platformio/penv/bin/pio run -e mega` — Expected: all pass, Mega builds (static_asserts on the EEPROM layout hold).
- [ ] **Step 5: Commit** `feat(protocol): track_balance setting (id 66) + full firmware slot (plan J task 1)`.

---

### Task 2: Lite setting with migration of the previous EEPROM block

**Files:** Modify `firmware/uno/src/domain/lite/settings_lite.h/.cpp`; Test `firmware/uno/test/test_lite_settings/test_main.cpp`.

**Interfaces:** Produces `LiteSettings::track_balance` (int16, ‰), get/set id 66 with range −100…100.

- [ ] **Step 1: Failing tests** (append, add `RUN_TEST`s):
```cpp
void test_settings_track_balance_range() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kTrackBalance), 19));
  int32_t v = 0;
  TEST_ASSERT_TRUE(s.get(id(proto::SettingId::kTrackBalance), v));
  TEST_ASSERT_EQUAL_INT32(19, v);
  TEST_ASSERT_EQUAL(proto::AckStatus::kSettingOutOfRange, s.set(id(proto::SettingId::kTrackBalance), 101));
}

// Review focus 1: a block saved before plan J (no track_balance) still loads; the new field takes its default.
void test_settings_previous_block_still_loads() {
  SettingsLite s;
  s.defaults();
  s.set(id(proto::SettingId::kEncodersEnabled), 1);
  const uint8_t old_size = static_cast<uint8_t>(offsetof(LiteSettings, track_balance));
  uint8_t full[SettingsLite::kBlockSize];
  s.encode(full);
  g_mem[0] = SettingsLite::kMagic0;
  g_mem[1] = SettingsLite::kMagic1;
  g_mem[2] = old_size;
  memcpy(g_mem + 3, full + 3, old_size);
  g_mem[3 + old_size] = proto::crc8(g_mem + 2, static_cast<uint8_t>(old_size + 1));
  SettingsLite r;
  TEST_ASSERT_TRUE(r.load(rd));
  TEST_ASSERT_EQUAL_UINT8(1, r.values().encoders_enabled);
  TEST_ASSERT_EQUAL_INT16(0, r.values().track_balance);
}
```
- [ ] **Step 2:** run `-f test_lite_settings` — Expected: compile error `no member named 'track_balance'` / `kTrackBalance` used.
- [ ] **Step 3: Implement** — append `int16_t track_balance;  // ‰, spec 02 §F5.1` to `LiteSettings`; `defaults()`: `m_s.track_balance = 0;`; `get`: `case Id::kTrackBalance: v = m_s.track_balance; return true;`; `set`: `case Id::kTrackBalance: if (!inRange(v, -100, 100)) return kRange; m_s.track_balance = static_cast<int16_t>(v); return kOk;`. In `load()` replace the size check and copy with:
```cpp
  const uint8_t stored = read(kAddr + 2);
  constexpr uint8_t kPrevSize = static_cast<uint8_t>(offsetof(LiteSettings, track_balance));  // before plan J
  if (read(kAddr) != kMagic0 || read(kAddr + 1) != kMagic1 || (stored != sizeof(LiteSettings) && stored != kPrevSize)) {
    return false;
  }
  uint8_t buf[sizeof(LiteSettings) + 1];
  buf[0] = stored;
  for (uint8_t i = 0; i < stored; ++i) buf[i + 1] = read(static_cast<uint16_t>(kAddr + 3 + i));
  if (proto::crc8(buf, static_cast<uint8_t>(stored + 1)) != read(static_cast<uint16_t>(kAddr + 3 + stored))) return false;
  SettingsLite loaded;
  loaded.defaults();  // fields not in an older block keep their defaults
  memcpy(&loaded.m_s, buf + 1, stored);
```
(the existing range-check loop and `m_s = loaded.m_s;` follow unchanged; add `#include <stddef.h>`).
- [ ] **Step 4:** run `-f test_lite_settings` then the full native suite — Expected: all pass.
- [ ] **Step 5: Commit** `feat(uno): Lite track_balance setting; a pre-plan-J EEPROM block still loads (plan J task 2)`.

---

### Task 3: `balanceTracks()` feed-forward and the sync ratio target

**Files:** Modify `firmware/uno/src/domain/lite/drive_mix_lite.h/.cpp`, `track_sync_lite.h/.cpp`; Test `firmware/uno/test/test_lite_encoders/test_main.cpp`.

**Interfaces:** Produces `Tracks balanceTracks(Tracks t, int16_t balance_pm)`; `TrackSyncLite::apply(Tracks, bool, int32_t, int32_t, uint16_t, uint16_t, int16_t balance_pm = 0)`.

- [ ] **Step 1: Failing tests:**
```cpp
void test_balance_feed_forward() {
  Tracks t = balanceTracks(Tracks{40, 40}, 20);  // right asked 2 % more: 40*(990/1000)=39.6 -> 39, 40*1010/1000 = 40.4 -> 40
  TEST_ASSERT_EQUAL_INT16(39, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
  t = balanceTracks(Tracks{80, 80}, 20);
  TEST_ASSERT_EQUAL_INT16(79, t.l);
  TEST_ASSERT_EQUAL_INT16(80, t.r);
  t = balanceTracks(Tracks{-80, -80}, 20);  // reverse: magnitudes, right still more
  TEST_ASSERT_EQUAL_INT16(-79, t.l);
  TEST_ASSERT_EQUAL_INT16(-80, t.r);
  t = balanceTracks(Tracks{100, 100}, 100);  // clamp
  TEST_ASSERT_TRUE(t.r <= 100);
  t = balanceTracks(Tracks{40, -40}, 20);  // spin: untouched
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(-40, t.r);
}

void test_sync_ratio_target() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 1000, 1019, 19);
  Tracks t = s.apply(Tracks{40, 40}, true, 1000, 1019, 1000, 1019, 19);  // right 1.9 % more = target: no correction
  TEST_ASSERT_EQUAL_INT16(0, s.correctionPct());
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  t = s.apply(Tracks{40, 40}, true, 2000, 2000, 1000, 1000, 19);  // equal counts: right is behind its target
  TEST_ASSERT_TRUE(s.correctionPct() > 0);                         // left slowed / right sped up
  TEST_ASSERT_TRUE(t.r >= t.l);
}
```
- [ ] **Step 2:** run `-f test_lite_encoders` — Expected: compile error `balanceTracks` undeclared / too many arguments.
- [ ] **Step 3: Implement** — in `drive_mix_lite.h` declare `Tracks balanceTracks(Tracks t, int16_t balance_pm);`; in `.cpp`:
```cpp
// Spec 02 §F5.1: straight driving only (both targets the same sign) - the right track asked balance ‰ more.
Tracks balanceTracks(Tracks t, int16_t balance_pm) {
  if (balance_pm == 0 || t.l == 0 || t.r == 0 || ((t.l > 0) != (t.r > 0))) return t;
  const int32_t fl = 1000 - balance_pm / 2, fr = 1000 + (balance_pm - balance_pm / 2);
  int32_t l = static_cast<int32_t>(t.l < 0 ? -t.l : t.l) * fl / 1000;
  int32_t r = static_cast<int32_t>(t.r < 0 ? -t.r : t.r) * fr / 1000;
  if (l > 100) l = 100;
  if (r > 100) r = 100;
  t.l = static_cast<int16_t>(t.l < 0 ? -l : l);
  t.r = static_cast<int16_t>(t.r < 0 ? -r : r);
  return t;
}
```
In `TrackSyncLite::apply` add the parameter `int16_t balance_pm` (default 0 in the header) and replace the error terms with:
```cpp
  const int32_t dl = count_l - m_base_l, dr = count_r - m_base_r;
  int32_t ahead = (dl * (1000 + balance_pm) - dr * 1000) / 1000;  // > 0: left ahead of its share
  // ... (existing clamp of ahead)
  const int32_t dcps = (static_cast<int32_t>(cps_l) * (1000 + balance_pm) - static_cast<int32_t>(cps_r) * 1000) / 1000;
  int32_t c = ahead / kSyncCountsPerPct + dcps / kSyncCpsPerPct;
```
- [ ] **Step 4:** run the full native suite — Expected: all pass (`balance_pm = 0` keeps every Plan I test).
- [ ] **Step 5: Commit** `feat(uno): track balance feed-forward and sync ratio target (plan J task 3)`.

---

### Task 4: Integral term in the heading hold

**Files:** Modify `firmware/uno/src/domain/lite/heading_hold_lite.h/.cpp`; Test `firmware/uno/test/test_lite_encoders/test_main.cpp`.

**Interfaces:** Unchanged signatures; `HeadingHoldLite::kIDivTicks = 400`, `kIMaxPct = 15`; private `int32_t m_i`.

- [ ] **Step 1: Failing tests:**
```cpp
namespace {
// Plant: a steady bias turns the tank right; w_ctrl (%) turns it left at 2 °/s per %. 10 ms ticks, 0.01° units.
int32_t simulateHold(HeadingHoldLite& h, int ticks, int16_t bias_pct) {
  int32_t yaw = 10000;  // 100.00°
  int8_t ws = 0, wc = 0;
  for (int i = 0; i < ticks; ++i) {
    const int16_t rate_ddps = static_cast<int16_t>(20 * (wc - bias_pct));  // 0.1 °/s
    yaw += rate_ddps / 10;                                                   // 0.01° per 10 ms
    h.teleop(40, 0, true, yaw, rate_ddps, ws, wc);
  }
  return h.errorCdeg();
}
}  // namespace

void test_hold_integral_removes_steady_error() {
  HeadingHoldLite h;
  h.configure(headingGainsFromMilli(3500, 1200));
  const int32_t err = simulateHold(h, 3000, 4);  // 30 s against a 4 % bias
  TEST_ASSERT_TRUE(err < 10 && err > -10);       // within 0.1° (P alone parks at ~4 / 6.15 = 0.65°)
}

void test_hold_integral_resets_on_steer_and_stop() {
  HeadingHoldLite h;
  h.configure(headingGainsFromMilli(3500, 1200));
  simulateHold(h, 1000, 4);
  int8_t ws = 0, wc = 0;
  h.teleop(40, 30, true, 10000, 0, ws, wc);  // steering: hold off
  h.teleop(40, 0, true, 10000, 0, ws, wc);   // new capture
  TEST_ASSERT_EQUAL_INT8(0, wc);             // no integral carried over
  simulateHold(h, 1000, 4);
  h.teleop(0, 0, true, 10000, 0, ws, wc);    // stop
  h.teleop(40, 0, true, 10000, 0, ws, wc);
  TEST_ASSERT_EQUAL_INT8(0, wc);
}

void test_hold_integral_settles_without_saturation() {
  HeadingHoldLite h;
  h.configure(headingGainsFromMilli(3500, 1200));
  int32_t yaw = 10000;
  int8_t ws = 0, wc = 0;
  int8_t peak = 0;
  for (int i = 0; i < 3000; ++i) {
    const int16_t rate_ddps = static_cast<int16_t>(20 * (wc - 4));
    yaw += rate_ddps / 10;
    h.teleop(40, 0, true, yaw, rate_ddps, ws, wc);
    const int8_t a = wc < 0 ? static_cast<int8_t>(-wc) : wc;
    if (a > peak) peak = a;
  }
  TEST_ASSERT_TRUE(peak < 100);  // never saturates (the Plan I runaway guard needs saturation)
}
```
- [ ] **Step 2:** run `-f test_lite_encoders` — Expected: `test_hold_integral_removes_steady_error` FAILS (P alone ≈ 65 cdeg).
- [ ] **Step 3: Implement** — `heading_hold_lite.h`: `static constexpr int32_t kIDivTicks = 400;  // Ki = Kp / 4 per second` and `static constexpr int32_t kIMaxPct = 15;`, private `int32_t m_i = 0;`. In `.cpp`: `m_i = 0;` in `reset()`, in the steering branch, and where the heading is captured (`m_held_cdeg = yaw_cdeg;`). Replace the output line with:
```cpp
  m_i += err;  // 0.01° × ticks
  if (m_g.kp_q8 > 0) {
    const int32_t lim = kIMaxPct * 25600 * kIDivTicks / m_g.kp_q8;  // |Ki · m_i| <= 15 %
    if (m_i > lim) m_i = lim;
    if (m_i < -lim) m_i = -lim;
  } else {
    m_i = 0;
  }
  int32_t out = static_cast<int32_t>(m_g.kp_q8) * err / 25600 +
                static_cast<int32_t>(m_g.kp_q8) * m_i / (25600 * kIDivTicks) -
                static_cast<int32_t>(m_g.kd_q8) * rate_ddps / 2560;
```
- [ ] **Step 4:** run the full native suite — Expected: all pass (incl. Plan I runaway tests).
- [ ] **Step 5: Commit** `feat(uno): heading hold integral removes the steady error (plan J task 4)`.

---

### Task 5: Wire into the Lite and build

**Files:** Modify `firmware/uno/src/lite/lite_control.cpp` (`driveTargets`).

- [ ] **Step 1:** in `driveTargets()` after `Tracks t = liteMix(...)` and before the `straight` computation:
```cpp
  if (w_steer == 0 && !g_lite.teleop_auto) t = balanceTracks(t, s.track_balance);  // spec 02 §F5.1
```
and pass `s.track_balance` as the last argument of `g_lite.sync.apply(...)`.
- [ ] **Step 2:** `~/.platformio/penv/bin/pio run -e uno -e mega && ~/.platformio/penv/bin/pio test -e native` — Expected: builds within budget (record flash / RAM), all tests pass.
- [ ] **Step 3: Commit** `feat(uno): track balance and the hold integral wired into the Lite (plan J task 5)`.

---

### Task 6: `straight_report.py`

**Files:** Create `tools/smoke/straight_report.py`.

- [ ] **Step 1:** write the tool (typed, stdlib only):
```python
"""Straight-run report from a td_record.py CSV (spec 02 §F5.3): heading profile, drift estimate, suggested track_balance.

    uv run python straight_report.py run.csv
"""

from __future__ import annotations

import csv
import math
import sys
from pathlib import Path


def wrap(d: float) -> float:
    return (d + 540.0) % 360.0 - 180.0


def main(path: Path) -> int:
    with path.open() as f:
        rows = list(csv.DictReader(f))
    moving = [r for r in rows if int(r["pwm_l"]) != 0 or int(r["pwm_r"]) != 0]
    if len(moving) < 10:
        print("no driving in this recording")
        return 1
    y0 = float(moving[0]["yaw"])
    n = len(moving)
    marks = [(p, wrap(float(moving[min(n - 1, int(n * p) - 1)]["yaw"]) - y0)) for p in (0.25, 0.5, 0.75, 1.0)]
    print("heading:", "  ".join(f"{int(p * 100)}%: {h:+.2f}°" for p, h in marks))
    dl = int(moving[-1]["enc_l"]) - int(moving[0]["enc_l"])
    dr = int(moving[-1]["enc_r"]) - int(moving[0]["enc_r"])
    cm = (dl + dr) / 2 / 75.6  # design-notes §63
    lateral = 0.0
    prev = int(moving[0]["enc_l"]) + int(moving[0]["enc_r"])
    for r in moving[1:]:
        cur = int(r["enc_l"]) + int(r["enc_r"])
        lateral += (cur - prev) / 2 / 75.6 * math.sin(math.radians(wrap(float(r["yaw"]) - y0)))
        prev = cur
    print(f"distance ≈ {cm:.0f} cm, sideways ≈ {lateral:+.1f} cm (+ = left), counts L {dl} R {dr}")
    if dl > 0:
        print(f"suggested track_balance ≈ {round((dr / dl - 1) * 1000)} ‰ (from this run's count ratio)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1])))
```
- [ ] **Step 2:** run it on an existing recording (`/tmp/long_gyro.csv` if present, else record a new idle 3 s file) — Expected: prints the profile, distance ≈ 310 cm and a suggested balance ≈ +19 ‰ for the 2026-10-17 normal run; `uv run ruff check straight_report.py && uv run pyright straight_report.py` clean.
- [ ] **Step 3: Commit** `feat(tools): straight_report.py - heading profile, drift and suggested track_balance (plan J task 6)`.

---

### Task 7: Tank (owner present, floor, ≥ 3.5 m tape)

- [ ] **Step 1:** flash the Uno (USB, S1 upload → back, unplug). This flash also carries the Plan I review fixes (I1, M1).
- [ ] **Step 2: Baseline with the integral only** (`track_balance` = 0): three 16 s runs at 40 % recorded with `td_record.py`, reported with `straight_report.py`, owner measures the drift.
- [ ] **Step 3: Balance:** set `track_balance` to the suggested value via the bench port (SETTING_SET, then SETTINGS_SAVE once good), three more runs.
- [ ] **Step 4:** acceptance — ≤ 1 cm sideways over ~3.1 m on three runs, heading within ±0.5° after the first second; otherwise adjust `kIDivTicks` or the balance one step at a time (each change recorded).
- [ ] **Step 5:** quick re-check of the latch (held track) and the review fixes; record everything in design-notes; commit `docs: plan J floor results`.

### Task 8: Final review and merge
- [ ] Fresh reviewer on the whole branch; Critical / Important fixed test-first; minors listed; ask the owner before merging to main.
