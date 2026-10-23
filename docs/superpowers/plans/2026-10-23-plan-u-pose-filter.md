# Plan U — PoseFilter revision 2 (map phase 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The AI engine knows where the tank is, which way it faces and how fast it moves, 50 times a second, from the tank's corrected heading and the encoders. A `cq-pose` tool replays a recording and says where the tank ended, for the floor check.

**Architecture:**
- **`domain/pose.py` (pure):**
  - `PoseFilter`: unwraps `td.yaw`; adds (Δl + Δr) / 2 / `counts_per_m` along θ_mid, with each track's direction from the previous sample's PWM sign; re-anchors on restarts and gaps.
  - `PoseHistory`: the last 2 s, interpolated.
- **`TelemSample`** gains `pwm_l` / `pwm_r`, read by `parse_td`.
- **`app/pose_replay.py`** (`cq-pose`) runs a recorded session through the filter.

**Tech Stack:** Python 3.12 (uv), pytest, ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §14 (approved 2026-10-23); design-notes §132; Spec 7 §4 (PoseFilter), §7 (pose drift targets), §9 phase 3.

**Built from a tested draft:** branch `u-draft`. AI engine 102 tests pass; ruff and pyright are clean.

## Global Constraints
- **Python:** 3.12 via uv, type hints everywhere, ruff (line ≤ 110) and pyright strict clean. Domain code is pure (no I/O).
- **The engine never sends motion** (Spec 7 §1). Module 1 only watches; on the floor the phone or Logo drives.
- **Room frame (LLD §3):** start (0, 0, 0); x forward at the start, y left, θ counter-clockwise. `td.yaw` is counter-clockwise positive (protocol spec 01).
- **Constants:**
  - `GAP_S` 0.5, `GAP_SPEED_MPS` 0.3;
  - `DRIFT_DIST` 0.03, `DRIFT_DEG_PER_TURN` 1.0;
  - `SPEED_WINDOW_US` 200 000, `HISTORY_US` 2 000 000;
  - stall bits `FastFlagBit` STALL_L 3 / STALL_R 4.
- **Hardware:**
  - floor runs with the owner present and STOP in hand ("stand or floor?" asked first, saying what moves);
  - recordings stay in `ai-engine/recordings/` (git-ignored) and are deleted after the numbers are written down (they show the home).

## Review Focus
1. **A turn that ends at a change of direction** (a Logo square's corner: forward → pivot): the counts of the last 20 ms belong to the previous output. Covered by `test_the_counts_go_with_the_output_they_happened_under`.
2. **The Uno's counts are 32-bit and only go up;** a restart resets them to 0. The `max(0, Δ)` guard and the `uno_ms` check must not turn a restart into a huge step. Covered by `test_an_uno_restart_keeps_the_pose`.
3. **The bridge's heading re-anchors itself** when the Uno restarts (bridge LLD R19): the yaw jumps on the same sample whose `uno_ms` went back, and the filter must not count that jump as a turn. Covered by the same test (yaw 0 on the restart sample).
4. **A `calib` reply that arrives after the first `td` samples** (`hello` → `debug` → `calib`): the early samples hold x, y and θ still updates; nothing breaks. Covered by `test_no_counts_per_m_holds_the_position` (the filter) and the replay summary (calib first in the fixture; a late calib is the same code path).
5. **`PoseHistory.at` at exactly the newest time** must answer, not None (a frame stamped with the newest pose's time), and a time older than 2 s must be None. Covered by `test_history_keeps_two_seconds`.

---

### Task 1: The pose filter and its history

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/pose.py`, `ai-engine/tests/test_pose.py`
- Modify: `ai-engine/src/conqueror_ai/domain/types.py` (`TelemSample.pwm_l`, `pwm_r`, default 0), `ai-engine/src/conqueror_ai/domain/telemetry.py` (`parse_td` reads `pwm`)

**Interfaces:**
- Produces:
  - `PoseFilter()`: `.set_counts_per_m(n: int)`, `.update(s: TelemSample) -> Pose`, `.pose() -> Pose | None`, `.drift() -> tuple[float, float]` (m, deg);
  - `PoseHistory()`: `.push(p: Pose)`, `.at(t_us: int) -> Pose | None`;
  - `TelemSample.pwm_l` / `pwm_r: int`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/ai-engine/tests/test_pose.py b/ai-engine/tests/test_pose.py
new file mode 100644
index 0000000..e78575c
--- /dev/null
+++ b/ai-engine/tests/test_pose.py
@@ -0,0 +1,202 @@
+"""PoseFilter revision 2 and PoseHistory (AI engine LLD §14, design-notes §132): the tank's corrected heading
+plus the encoders, with the PWM sign per track."""
+
+from __future__ import annotations
+
+import math
+from dataclasses import replace
+
+from conqueror_ai.domain.pose import PoseFilter, PoseHistory
+from conqueror_ai.domain.telemetry import parse_td
+from conqueror_ai.domain.types import Pose, TelemSample
+
+CPM = 7514
+STALL_L, STALL_R = 1 << 3, 1 << 4
+
+
+def td(
+    t_ms: float,
+    enc: tuple[int, int],
+    pwm: tuple[int, int],
+    yaw: float,
+    uno_ms: int | None = None,
+    flags: int = 0,
+) -> TelemSample:
+    """A td sample at bridge time t_ms (the Uno's clock runs alongside unless given)."""
+    return TelemSample(
+        bus_us=int(t_ms * 1000),
+        uno_ms=int(t_ms) if uno_ms is None else uno_ms,
+        enc_l=enc[0],
+        enc_r=enc[1],
+        yaw_deg=yaw % 360.0,
+        rate_dps=0.0,
+        dist_mm=None,
+        pan_deg=90.0,
+        tilt_deg=90.0,
+        cmd_l=0,
+        cmd_r=0,
+        flags=flags,
+        pwm_l=pwm[0],
+        pwm_r=pwm[1],
+    )
+
+
+def drive(
+    f: PoseFilter,
+    steps: int,
+    d_counts: tuple[int, int],
+    pwm: tuple[int, int],
+    dyaw: float = 0.0,
+    t0: float = 0.0,
+    start: tuple[int, int] = (0, 0),
+    yaw0: float = 0.0,
+) -> Pose:
+    """`steps` samples 20 ms apart, each adding d_counts and dyaw; returns the last pose."""
+    p = None
+    for i in range(steps + 1):
+        p = f.update(
+            td(t0 + 20 * i, (start[0] + d_counts[0] * i, start[1] + d_counts[1] * i), pwm, yaw0 + dyaw * i)
+        )
+    assert p is not None
+    return p
+
+
+def new() -> PoseFilter:
+    f = PoseFilter()
+    f.set_counts_per_m(CPM)
+    return f
+
+
+def test_straight_one_metre() -> None:
+    p = drive(new(), 100, (75, 75), (120, 120))  # 7500 counts on each track
+    assert math.isclose(p.x_m, 7500 / CPM, abs_tol=1e-9) and abs(p.y_m) < 1e-9 and p.th_rad == 0.0
+
+
+def test_backwards() -> None:
+    p = drive(new(), 100, (75, 75), (-120, -120))
+    assert math.isclose(p.x_m, -7500 / CPM, abs_tol=1e-9)
+
+
+def test_a_left_turn_while_driving_bends_the_path_left() -> None:
+    p = drive(new(), 90, (75, 75), (120, 120), dyaw=1.0)  # 90 deg to the left over ~0.9 m
+    assert math.isclose(math.degrees(p.th_rad), 90.0, abs_tol=1e-6)
+    assert p.y_m > 0.5 and p.x_m > 0.5
+
+
+def test_a_pivot_goes_nowhere() -> None:
+    p = drive(new(), 50, (40, 40), (120, -120), dyaw=1.8)
+    assert abs(p.x_m) < 1e-9 and abs(p.y_m) < 1e-9
+    assert math.isclose(math.degrees(p.th_rad), 90.0, abs_tol=1e-6)
+
+
+def test_coasting_keeps_the_last_direction() -> None:
+    f = new()
+    drive(f, 10, (75, 75), (-120, -120))
+    p = drive(f, 10, (75, 75), (0, 0), t0=220, start=(750, 750))  # motor off, tracks still rolling back
+    assert math.isclose(p.x_m, -(750 + 750) / CPM, abs_tol=1e-9)
+
+
+def test_a_stalled_track_adds_nothing() -> None:
+    f = new()
+    p = None
+    for i in range(11):
+        p = f.update(td(20 * i, (75 * i, 75 * i), (120, 120), 0.0, flags=STALL_L | STALL_R))
+    assert p is not None and p.x_m == 0.0
+
+
+def test_the_heading_wraps_without_a_jump() -> None:
+    p = drive(new(), 4, (0, 0), (0, 0), dyaw=0.5, yaw0=359.0)  # 359 -> 1 deg
+    assert math.isclose(math.degrees(p.th_rad), 2.0, abs_tol=1e-6)
+
+
+def test_an_uno_restart_keeps_the_pose() -> None:
+    f = new()
+    before = drive(f, 50, (75, 75), (120, 120), dyaw=0.4)
+    # the Uno restarts: its clock, counts and heading start again from 0
+    after = f.update(td(1020, (0, 0), (0, 0), 0.0, uno_ms=5))
+    assert after is not None
+    assert (after.x_m, after.y_m, after.th_rad) == (before.x_m, before.y_m, before.th_rad)
+    nxt = f.update(td(1040, (75, 75), (120, 120), 0.0, uno_ms=25))
+    assert nxt is not None and nxt.x_m > before.x_m  # and it carries on from there
+
+
+def test_a_gap_is_not_stepped_across_and_grows_the_drift() -> None:
+    f = new()
+    x = drive(f, 10, (75, 75), (120, 120)).x_m
+    d0 = f.drift()[0]
+    p = f.update(td(1200, (5000, 5000), (120, 120), 0.0))  # 1 s silent, then far further on
+    assert p is not None and p.x_m == x
+    assert f.drift()[0] >= d0 + 0.3 - 1e-9
+
+
+def test_no_counts_per_m_holds_the_position() -> None:
+    p = drive(PoseFilter(), 50, (75, 75), (120, 120), dyaw=0.2)
+    assert p.x_m == 0.0 and p.y_m == 0.0 and math.isclose(math.degrees(p.th_rad), 10.0, abs_tol=1e-6)
+
+
+def test_the_drift_grows_with_the_distance_and_the_turn() -> None:
+    f = new()
+    drive(f, 500, (75, 75), (120, 120))  # ~5 m
+    assert math.isclose(f.drift()[0], 0.03 * 500 * 150 / 2 / CPM, rel_tol=1e-6)
+    f2 = new()
+    drive(f2, 360, (0, 0), (120, -120), dyaw=1.0)  # one full turn on the spot
+    assert math.isclose(f2.drift()[1], 1.0, rel_tol=1e-6)
+
+
+def test_the_speed_and_turn_rate() -> None:
+    f = new()
+    p = drive(f, 50, (75, 75), (120, 120))
+    assert math.isclose(p.v_mps, 75 / CPM / 0.02, rel_tol=1e-6)
+    s = td(1020, (3825, 3825), (120, 120), 0.0)
+    q = f.update(replace(s, rate_dps=90.0))
+    assert q is not None and math.isclose(q.w_rps, math.pi / 2)
+
+
+def test_history_interpolates_and_knows_its_window() -> None:
+    h = PoseHistory()
+    h.push(Pose(1_000_000, 0.0, 0.0, math.radians(179.0), 0.0, 0.0))
+    h.push(Pose(1_020_000, 0.2, 0.0, math.radians(-179.0), 0.0, 0.0))
+    p = h.at(1_010_000)
+    assert p is not None and math.isclose(p.x_m, 0.1)
+    assert math.isclose(abs(math.degrees(p.th_rad)), 180.0, abs_tol=1e-6)  # across +-180, not through 0
+    assert h.at(900_000) is None and h.at(1_030_000) is None
+
+
+def test_history_keeps_two_seconds() -> None:
+    h = PoseHistory()
+    for i in range(150):
+        h.push(Pose(i * 20_000, float(i), 0.0, 0.0, 0.0, 0.0))
+    assert h.at(0) is None and h.at(149 * 20_000) is not None
+    oldest = h.at(50 * 20_000)
+    assert oldest is not None and oldest.x_m == 50.0
+
+
+def test_the_parser_reads_the_pwm() -> None:
+    s = parse_td(
+        {
+            "t": "td",
+            "seq": 1,
+            "uno_ms": 5,
+            "bus_us": 9,
+            "enc": [1, 2],
+            "pwm": [-80, 95],
+            "cmd": [0, 0],
+            "yaw": 1.0,
+            "rate": 0.0,
+            "dist_mm": None,
+            "pan": 90.0,
+            "tilt": 90.0,
+            "flags": 0,
+        }
+    )
+    assert s is not None and (s.pwm_l, s.pwm_r) == (-80, 95)
+
+
+def test_the_counts_go_with_the_output_they_happened_under() -> None:
+    """A sample's count change happened during the 20 ms before it, under the previous sample's PWM."""
+    f = new()
+    f.update(td(0, (0, 0), (120, 120), 0.0))
+    p = f.update(
+        td(20, (75, 75), (120, -120), 0.0)
+    )  # the output just changed to a pivot; the 75 were forward
+    assert math.isclose(p.x_m, 75 / CPM, abs_tol=1e-12)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_pose.py`
Expected: `ModuleNotFoundError: No module named 'conqueror_ai.domain.pose'`.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/ai-engine/src/conqueror_ai/domain/pose.py b/ai-engine/src/conqueror_ai/domain/pose.py
new file mode 100644
index 0000000..281c3bb
--- /dev/null
+++ b/ai-engine/src/conqueror_ai/domain/pose.py
@@ -0,0 +1,124 @@
+"""Where the tank is (AI engine LLD §14, design-notes §132). Pure: telemetry samples in, poses out.
+
+- Heading: the tank's own, as `td` sends it - the Uno re-learns the gyro bias at rest and the bridge applies
+  the gyro scale, so it is used as it comes; unwrapped across 360, the room's 0 at the first sample. Counter-
+  clockwise positive, like the room's theta.
+- Distance: the mean of the two tracks' encoder counts / counts_per_m. The encoders have one channel, so each
+  track's direction is the sign of its PWM output - the output of the sample before, because a sample's count
+  change happened during the 20 ms before it - and the last non-zero sign while it coasts; a stalled track
+  adds nothing.
+- A restart (the Uno's clock or the bridge's going back) or a gap over 0.5 s re-anchors: no step, no jump."""
+
+from __future__ import annotations
+
+import math
+from collections import deque
+
+from conqueror_ai.domain.types import Pose, TelemSample
+
+GAP_S = 0.5  # longer without a sample: no step across it
+GAP_SPEED_MPS = 0.3  # the drift a gap may hide: the tank's fastest straight speed x the gap
+DRIFT_DIST = 0.03  # of the distance driven (Spec 7 §7)
+DRIFT_DEG_PER_TURN = 1.0  # per 360 deg turned
+SPEED_WINDOW_US = 200_000
+HISTORY_US = 2_000_000
+STALL_L, STALL_R = 1 << 3, 1 << 4  # FastFlagBit STALL_L / STALL_R
+
+
+def _wrap180(d: float) -> float:
+    return (d + 180.0) % 360.0 - 180.0
+
+
+class PoseFilter:
+    def __init__(self) -> None:
+        self._cpm: int | None = None
+        self._last: TelemSample | None = None
+        self._sign = [0, 0]  # the last non-zero PWM sign per track
+        self._x = self._y = self._th_deg = 0.0
+        self._steps: deque[tuple[int, float]] = deque()  # (t_us, d) for the speed
+        self._drift_m = 0.0
+        self._drift_deg = 0.0
+        self._pose: Pose | None = None
+
+    def set_counts_per_m(self, n: int) -> None:
+        self._cpm = n
+
+    def pose(self) -> Pose | None:
+        return self._pose
+
+    def drift(self) -> tuple[float, float]:
+        """How far off the pose could be: (m, deg)."""
+        return self._drift_m, self._drift_deg
+
+    def update(self, s: TelemSample) -> Pose:
+        last, self._last = self._last, s
+        d = 0.0
+        if last is not None and not self._restarted(last, s):
+            gap_s = (s.bus_us - last.bus_us) / 1e6
+            if gap_s > GAP_S:
+                self._drift_m += GAP_SPEED_MPS * gap_s
+            else:
+                d = self._step(last, s)
+        for i, pwm in enumerate(
+            (s.pwm_l, s.pwm_r)
+        ):  # after the step: its counts ran under the earlier output
+            if pwm != 0:
+                self._sign[i] = 1 if pwm > 0 else -1
+        self._steps.append((s.bus_us, d))
+        while self._steps and s.bus_us - self._steps[0][0] > SPEED_WINDOW_US:
+            self._steps.popleft()
+        span_us = s.bus_us - self._steps[0][0]
+        moved = sum(x for _, x in list(self._steps)[1:])  # the oldest one's step lies before the window
+        v = moved / (span_us / 1e6) if span_us > 0 else 0.0
+        self._pose = Pose(s.bus_us, self._x, self._y, math.radians(self._th_deg), v, math.radians(s.rate_dps))
+        return self._pose
+
+    @staticmethod
+    def _restarted(last: TelemSample, s: TelemSample) -> bool:
+        return s.uno_ms < last.uno_ms or s.bus_us < last.bus_us
+
+    def _step(self, last: TelemSample, s: TelemSample) -> float:
+        dth = _wrap180(s.yaw_deg - last.yaw_deg)
+        th_mid = math.radians(self._th_deg + dth / 2.0)
+        self._th_deg += dth
+        self._drift_deg += abs(dth) / 360.0 * DRIFT_DEG_PER_TURN
+        if self._cpm is None:
+            return 0.0
+        dl = 0 if s.flags & STALL_L else max(0, s.enc_l - last.enc_l) * self._sign[0]
+        dr = 0 if s.flags & STALL_R else max(0, s.enc_r - last.enc_r) * self._sign[1]
+        d = (dl + dr) / 2.0 / self._cpm
+        self._x += d * math.cos(th_mid)
+        self._y += d * math.sin(th_mid)
+        self._drift_m += DRIFT_DIST * abs(d)
+        return d
+
+
+class PoseHistory:
+    """The last 2 s of poses; the pose at any time inside them, interpolated (theta on the circle)."""
+
+    def __init__(self) -> None:
+        self._poses: deque[Pose] = deque()
+
+    def push(self, p: Pose) -> None:
+        self._poses.append(p)
+        while p.t_us - self._poses[0].t_us > HISTORY_US:
+            self._poses.popleft()
+
+    def at(self, t_us: int) -> Pose | None:
+        ps = self._poses
+        if not ps or t_us < ps[0].t_us or t_us > ps[-1].t_us:
+            return None
+        for a, b in zip(ps, list(ps)[1:], strict=False):
+            if a.t_us <= t_us <= b.t_us:
+                f = 0.0 if b.t_us == a.t_us else (t_us - a.t_us) / (b.t_us - a.t_us)
+                dth = (b.th_rad - a.th_rad + math.pi) % (2 * math.pi) - math.pi
+                th = (a.th_rad + f * dth + math.pi) % (2 * math.pi) - math.pi
+                return Pose(
+                    t_us,
+                    a.x_m + f * (b.x_m - a.x_m),
+                    a.y_m + f * (b.y_m - a.y_m),
+                    th,
+                    a.v_mps + f * (b.v_mps - a.v_mps),
+                    a.w_rps + f * (b.w_rps - a.w_rps),
+                )
+        return ps[-1]
diff --git a/ai-engine/src/conqueror_ai/domain/telemetry.py b/ai-engine/src/conqueror_ai/domain/telemetry.py
index c3b18a1..59b2e8e 100644
--- a/ai-engine/src/conqueror_ai/domain/telemetry.py
+++ b/ai-engine/src/conqueror_ai/domain/telemetry.py
@@ -32,6 +32,7 @@ def parse_td(m: dict[str, Any]) -> TelemSample | None:
     try:
         enc_l, enc_r = _pair(m, "enc")
         cmd_l, cmd_r = _pair(m, "cmd")
+        pwm_l, pwm_r = _pair(m, "pwm")
         dist = m.get("dist_mm")
         return TelemSample(
             bus_us=int(_num(m, "bus_us")),
@@ -46,6 +47,8 @@ def parse_td(m: dict[str, Any]) -> TelemSample | None:
             cmd_l=cmd_l,
             cmd_r=cmd_r,
             flags=int(_num(m, "flags")),
+            pwm_l=pwm_l,
+            pwm_r=pwm_r,
         )
     except (KeyError, ValueError):
         return None
diff --git a/ai-engine/src/conqueror_ai/domain/types.py b/ai-engine/src/conqueror_ai/domain/types.py
index b267114..87d3b0f 100644
--- a/ai-engine/src/conqueror_ai/domain/types.py
+++ b/ai-engine/src/conqueror_ai/domain/types.py
@@ -21,6 +21,8 @@ class TelemSample:
     cmd_l: int
     cmd_r: int
     flags: int
+    pwm_l: int = 0  # the motor output, -255..255: its sign is the track's direction (LLD §14)
+    pwm_r: int = 0
 
 
 @dataclass(frozen=True)
```

- [ ] **Step 4: Run the tests and the checks**

Run: `cd ai-engine && uv run pytest -q && cd ai-engine && uv run ruff check . && cd ai-engine && uv run ruff format --check . && cd ai-engine && uv run pyright`
Expected: `16 passed` in `test_pose.py`, all tests passing (99), ruff and pyright clean.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/domain ai-engine/tests/test_pose.py
git commit -m "feat(ai): PoseFilter revision 2 - the tank's heading plus the encoders (AI engine LLD §14)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: `cq-pose` — a recording through the filter

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/pose_replay.py`, `ai-engine/tests/test_pose_replay.py`
- Modify: `ai-engine/pyproject.toml` (`cq-pose` script)

**Interfaces:**
- Consumes: Task 1's `PoseFilter`; `adapters.session.replay(folder)` (recordings keep every raw message, the `calib` reply included).
- Produces:
  - `summarize(folder) -> PoseSummary` (samples, counts_per_m, start, end, farthest_m, driven_m, turned_deg, drift_m, drift_deg);
  - `report(s) -> str`;
  - `NoCalibration`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/ai-engine/tests/test_pose_replay.py b/ai-engine/tests/test_pose_replay.py
new file mode 100644
index 0000000..62f3e6e
--- /dev/null
+++ b/ai-engine/tests/test_pose_replay.py
@@ -0,0 +1,75 @@
+"""`cq-pose`: a recorded session through the pose filter, for the floor check (AI engine LLD §14.5)."""
+
+from __future__ import annotations
+
+import json
+import math
+from pathlib import Path
+
+import pytest
+
+from conqueror_ai.adapters.session import SessionRecorder
+from conqueror_ai.app import pose_replay
+
+
+def td(t_ms: int, enc: tuple[int, int], pwm: tuple[int, int], yaw: float) -> str:
+    return json.dumps(
+        {
+            "t": "td",
+            "seq": t_ms,
+            "uno_ms": t_ms,
+            "bus_us": t_ms * 1000,
+            "enc": list(enc),
+            "pwm": list(pwm),
+            "cmd": [0, 0],
+            "yaw": yaw % 360.0,
+            "rate": 0.0,
+            "dist_mm": None,
+            "pan": 90.0,
+            "tilt": 90.0,
+            "flags": 0,
+        }
+    )
+
+
+def out_and_back(folder: Path, with_calib: bool = True) -> None:
+    """1 m out, a 180 deg pivot to the left, 1 m back."""
+    rec = SessionRecorder(folder, {"what": "test"})
+    if with_calib:
+        rec.telemetry(
+            0.0, json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27, "gyro_scale": 10310})
+        )
+    t, cl, r, yaw = 0, 0, 0, 0.0
+    for _ in range(100):  # out: 7514 counts in 100 steps
+        rec.telemetry(t / 1000, td(t, (cl, r), (120, 120), yaw))
+        t, cl, r = t + 20, cl + 75, r + 75
+    cl, r = cl + 14, r + 14  # the last 14 counts, so it is exactly 1 m
+    for _ in range(90):  # pivot left, 2 deg per step
+        rec.telemetry(t / 1000, td(t, (cl, r), (120, -120), yaw))
+        t, cl, r, yaw = t + 20, cl + 30, r + 30, yaw + 2.0
+    for _ in range(101):  # back
+        rec.telemetry(t / 1000, td(t, (cl, r), (120, 120), yaw))
+        t, cl, r = t + 20, cl + 75, r + 75
+    rec.close()
+
+
+def test_an_out_and_back_ends_near_the_start(tmp_path: Path) -> None:
+    out_and_back(tmp_path / "s")
+    s = pose_replay.summarize(tmp_path / "s")
+    assert math.isclose(s.farthest_m, 1.0, abs_tol=0.002)
+    assert math.hypot(s.end.x_m, s.end.y_m) < 0.01
+    assert math.isclose(s.turned_deg, 180.0, abs_tol=0.5)
+    assert math.isclose(s.driven_m, 2.0, abs_tol=0.02)
+    assert s.counts_per_m == 7514 and s.samples == 291
+
+
+def test_a_recording_without_counts_per_m_is_refused(tmp_path: Path) -> None:
+    out_and_back(tmp_path / "s", with_calib=False)
+    with pytest.raises(pose_replay.NoCalibration):
+        pose_replay.summarize(tmp_path / "s")
+
+
+def test_the_summary_reads_plainly(tmp_path: Path) -> None:
+    out_and_back(tmp_path / "s")
+    text = pose_replay.report(pose_replay.summarize(tmp_path / "s"))
+    assert "end" in text and "farthest" in text and "turned" in text and "cm" in text
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_pose_replay.py`
Expected: `ImportError: cannot import name 'pose_replay'`.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/ai-engine/pyproject.toml b/ai-engine/pyproject.toml
index c650c05..194292e 100644
--- a/ai-engine/pyproject.toml
+++ b/ai-engine/pyproject.toml
@@ -15,6 +15,7 @@ dependencies = [
 cq-record = "conqueror_ai.app.record:main"
 cq-replay = "conqueror_ai.app.replay:main"
 cq-calibrate = "conqueror_ai.app.calibrate:main"
+cq-pose = "conqueror_ai.app.pose_replay:main"
 
 [build-system]
 requires = ["hatchling"]
diff --git a/ai-engine/src/conqueror_ai/app/pose_replay.py b/ai-engine/src/conqueror_ai/app/pose_replay.py
new file mode 100644
index 0000000..270dacf
--- /dev/null
+++ b/ai-engine/src/conqueror_ai/app/pose_replay.py
@@ -0,0 +1,101 @@
+"""`cq-pose`: replay a recorded session through the pose filter and say where the tank ended - the floor check
+of AI engine LLD §14.5 (a Logo square ends at its start; a 2 m out-and-back turns at 2.00 m).
+
+    uv run cq-pose recordings/20261023-181500
+"""
+
+from __future__ import annotations
+
+import argparse
+import json
+import math
+from dataclasses import dataclass
+from pathlib import Path
+
+from conqueror_ai.adapters.session import replay
+from conqueror_ai.domain.pose import PoseFilter
+from conqueror_ai.domain.telemetry import parse_td
+from conqueror_ai.domain.types import Pose
+
+
+class NoCalibration(Exception):
+    """The recording has no `calib` reply, so the distance per encoder count is unknown."""
+
+
+@dataclass(frozen=True)
+class PoseSummary:
+    samples: int
+    counts_per_m: int
+    start: Pose
+    end: Pose
+    farthest_m: float  # the farthest the tank got from its start
+    driven_m: float  # the path's length
+    turned_deg: float  # all turning, either way
+    drift_m: float
+    drift_deg: float
+
+
+def summarize(folder: Path) -> PoseSummary:
+    f = PoseFilter()
+    cpm: int | None = None
+    start: Pose | None = None
+    prev: Pose | None = None
+    samples = 0
+    farthest = driven = turned = 0.0
+    for e in replay(folder):
+        if e.text is None:
+            continue
+        m = json.loads(e.text)
+        if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
+            cpm = int(m["counts_per_m"])
+            f.set_counts_per_m(cpm)
+            continue
+        s = parse_td(m)
+        if s is None:
+            continue
+        p = f.update(s)
+        samples += 1
+        if start is None:
+            start = p
+        if prev is not None:
+            driven += math.hypot(p.x_m - prev.x_m, p.y_m - prev.y_m)
+            turned += abs((p.th_rad - prev.th_rad + math.pi) % (2 * math.pi) - math.pi)
+        farthest = max(farthest, math.hypot(p.x_m - start.x_m, p.y_m - start.y_m))
+        prev = p
+    if cpm is None:
+        raise NoCalibration(
+            f"no calib reply in {folder} - record with cq-record, which asks the bridge for it"
+        )
+    if start is None or prev is None:
+        raise ValueError(f"no telemetry in {folder}")
+    dm, dd = f.drift()
+    return PoseSummary(samples, cpm, start, prev, farthest, driven, math.degrees(turned), dm, dd)
+
+
+def report(s: PoseSummary) -> str:
+    e = s.end
+    return "\n".join(
+        [
+            f"{s.samples} samples, counts_per_m {s.counts_per_m}",
+            f"end: x {e.x_m * 100:+.1f} cm, y {e.y_m * 100:+.1f} cm (left +), "
+            f"heading {math.degrees(e.th_rad):+.1f} deg",
+            f"end from the start: {math.hypot(e.x_m, e.y_m) * 100:.1f} cm",
+            f"farthest from the start: {s.farthest_m * 100:.1f} cm",
+            f"driven {s.driven_m * 100:.0f} cm, turned {s.turned_deg:.0f} deg",
+            f"could be off by: {s.drift_m * 100:.1f} cm, {s.drift_deg:.1f} deg",
+        ]
+    )
+
+
+def main() -> None:
+    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
+    ap.add_argument("folder", type=Path)
+    a = ap.parse_args()
+    try:
+        print(report(summarize(a.folder)))
+    except (NoCalibration, ValueError) as err:
+        raise SystemExit(str(err)) from None
+
+
+if __name__ == "__main__":
+    main()
```

- [ ] **Step 4: Run the tests and the checks** (`uv sync` first, so the new script is installed)

Run: `cd ai-engine && uv sync -q && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright && uv run cq-pose --help | head -2`
Expected: `102 passed`; ruff and pyright clean; the help text shows.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/app/pose_replay.py ai-engine/tests/test_pose_replay.py ai-engine/pyproject.toml
git commit -m "feat(ai): cq-pose - a recording through the pose filter, for the floor check (AI engine LLD §14.5)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: On the floor — square and out-and-back (owner present)

Ask "stand or floor?" first: **floor**. Say what moves: the tracks, driven by Logo; the engine only records.

- [ ] **Step 1: The square.**
  - Mark the start with tape: the tracks' front and the left edge.
  - Record: `cd ai-engine && uv run cq-record --seconds 60 --no-view --out recordings/u-square`.
  - On `/logo`, run `REPEAT 4 [ FORWARD 50 RIGHT 90 ]`.
  - The owner measures how far the tank ended from the tape (cm) and its heading against the start (by eye or a strip).
- [ ] **Step 2: The out-and-back.**
  - Record: `cd ai-engine && uv run cq-record --seconds 60 --no-view --out recordings/u-outback`.
  - Run `FORWARD 200 RIGHT 180 FORWARD 200`. The owner measures the far point (the tank's front at the turn, with a tape from the start) and the end against the start.
- [ ] **Step 3: Replay.**
  - Run `cd ai-engine && uv run cq-pose recordings/u-square` and `cd ai-engine && uv run cq-pose recordings/u-outback`.
  - Compare with the tape:
    - **square:** end from the start, and the heading;
    - **out-and-back:** farthest ≈ 200 cm, and the end.
  - Targets (Spec 7 §7): position ≤ 3 % of the distance driven (square 200 cm → 6 cm; out-and-back 400 cm → 12 cm); heading ≤ 3° per 360° turned.
- [ ] **Step 4:** Write the numbers down in the ledger, then delete both recordings (`rm -r ai-engine/recordings/u-square ai-engine/recordings/u-outback`).

### Task 4: Docs as built

**Files:**
- `docs/lld/ai-engine-lld.md` (§14.6 as built, the floor numbers);
- `docs/design-notes.md` (the results, with how each number was measured);
- `CLAUDE.md` (plan list: `2026-10-23-plan-u-pose-filter.md`; Next: phase 4, Detector + ObjectTracker).

- [ ] **Step 1: Write them.**
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan U pose filter - as built and floor results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
