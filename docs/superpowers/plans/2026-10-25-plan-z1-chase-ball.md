# Plan Z1 — Drive while thinking: chase the ball (laptop side) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The tank finds the ball and drives to it **in one smooth move**: no drive-stop-drive. A loop on the laptop thinks while the tank drives. This is slice 1 of module 2, run standalone as `cq-drive goto ball`. The Tank Logo words come in Plan Z2.

**Architecture:**
- **Pure parts** (`domain/drive/`), all host-tested:
  - `Limits`: what the tank can do smoothly;
  - `ramp` / `roll_out`: gentle changes, and where a command takes the tank;
  - `Planner`: a fan of curves, scored on progress, clearance and smoothness;
  - `Goal`: which ball, and where it is; tilt-free from its size;
  - `ChaseSkill`: looking, approaching, arrived or lost;
  - `SimTank`: so whole chases run in tests.
- **Adapters:**
  - `DriveLink`: the only part of the engine that moves the tank, through `drive_rate` on its own WebSocket. `BridgeLink` stays motion-free;
  - `drive_config` (`config/drive.yaml`).
- **App:** `cq-drive goto ball` runs Plans U, V and Y's pipeline, with the skill on top.

**Tech Stack:** Python 3.12 (uv, ruff, pyright strict, pytest), websockets; the existing pipeline (PoseFilter, ObjectPipeline, YOLO-World + colour finder).

**Spec:** `docs/superpowers/specs/2026-10-25-08-drive-while-thinking-design.md` (spec 8); AI engine LLD §17; design-notes §156.

**Built from a tested draft:** branch `z1-draft`.
- AI engine 227 passed, 1 skipped. Ruff, format and pyright clean.
- **Simulated chases** (the drive tests):
  - the ball 1.6 m ahead → arrives in 4.7 s;
  - 45° left → 6.3 s;
  - 90° right → 8.9 s;
  - behind → 11.1 s (camera sweep, one turn on the spot, then straight);
  - each stops ~44 cm from the lens to the ball, facing it within 10°, **never standing still on the way**.
  - A ball rolled away at 3 s is reached where it went, with no stop.
- **What the draft taught** (all in the code):
  - **Stop only when every curve is blocked.** Penalising change from standstill made "stay still" win.
  - **Near the slowest speed (18 cm/s) the tank can't curve at all,** because the inner track can't go slower. So cruise's arcs are always offered, and slowing is for the straight final approach.
  - **A target inside the tightest circle** (~33 cm at 25 cm/s) can't be reached by any forward curve. The tank turns on the spot to face it, and keeps turning until within 8° (no spin-drive-spin stutter).
  - **A spin is full (65 °/s) or none:** slower spins aren't smooth.
  - **Smoothness counts steering changes while driving.** Speed changes are the ramp's job.

## Global Constraints
- **Safety unchanged:**
  - nothing on the Uno or the bridge changes;
  - every command is an ordinary `drive_rate` through the Uno's safety supervisor (obstacle brake, deadman, link loss, stall, battery, child-play limits);
  - STOP from any device ends a run.
- **The engine refuses to drive** with a stale map (no picture for 0.5 s, or no telemetry for 0.2 s) or without a pose. Slice 1's speed cap is **25 cm/s**.
- **The tank's envelope** (spec 8 §2):
  - forward speeds 18–33 cm/s (0 or ≥ 18);
  - no arc under 31 cm radius;
  - `r_min(v) = max(0.31, 0.095·v/(v − 17.7))` m;
  - a spin in place is 65 °/s or none.
- **The ramps:** 10 cm/s per second and 60 °/s per second (the 0 ↔ 18 cm/s dead band, and a spin's start and end, in one step).
- **Arrival:** 40 ± 15 cm from the lens to the ball's centre, facing it within 10°.
- **Signs:**
  - turn rate + = left (counter-clockwise, bridge R4);
  - the camera's pan + = right (`cam_set`), so the camera follows the target with **pan = −bearing**.
- **The loop and the link:** the loop ticks on each used picture and at least every 0.2 s. `DriveLink` resends the command every 0.2 s, so the bridge's 0.3 s deadman fires only when the engine stalls.
- **Hardware:** floor checks only with the owner present and STOP in hand. Recordings show the home: keep them local and delete them after the numbers are written down.

## Review Focus
1. **The tank never stands still between the start and arrival** (the owner's end goal). Pinned by `test_a_chase_reaches_the_ball_in_one_smooth_move` (four directions) and the rolled-ball test.
2. **No command outside what the tank can do smoothly:** no speed between 0 and 18, no arc under `r_min`, no slow spin. Pinned by `test_the_fan_never_offers_what_the_tank_cant_do` and `test_commands_stay_inside_what_the_tank_can_do`.
3. **A stale map, no pose, or a refusal from the tank stops it at once.** Pinned by `test_a_stale_map_or_no_pose_ends_it_at_once`; the app checks `drive.refused` every tick.
4. **An echo that isn't the ball is kept clear of; the ball's own echo isn't an obstacle.** Pinned by `test_an_echo_ahead_that_is_not_the_target_is_kept_clear_of` and `test_an_echo_at_the_targets_range_straight_ahead_is_the_target`.
5. **Ctrl-C or an error ends the run with a STOP and `release_control`** (the app's `finally`). Checked on the floor in Task 6.

---

### Task 1: The limits, the ramp and the roll-out

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/__init__.py`, `.../drive/limits.py`, `.../drive/curves.py`
- Test: `ai-engine/tests/test_drive_limits.py`

**Interfaces:**
- Produces: `Limits` (fields as written, `r_min_m(v)`, `top_speed(d_lens)`); `Command(v_cms, w_dps)`, `STOP`, `XYTh`; `ramp(now, want, dt, lim) -> Command`; `roll_out(start, now, want, lim) -> list[XYTh]`.

- [ ] **Step 1: Write the tests**, `ai-engine/tests/test_drive_limits.py`:

```python
"""The tank's limits, the ramp and the roll-out (AI engine LLD §17.2, §17.4)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.drive.curves import STOP, Command, ramp, roll_out
from conqueror_ai.domain.drive.limits import Limits

LIM = Limits()


# ---- the limits (design-notes §71)
def test_the_tightest_smooth_arc_by_speed() -> None:
    assert math.isinf(LIM.r_min_m(17.0))
    assert LIM.r_min_m(18.0) > 1.5  # near the inner track's lowest speed: straight only
    assert LIM.r_min_m(20.0) == pytest.approx(0.826, abs=0.01)
    assert LIM.r_min_m(25.0) == pytest.approx(0.325, abs=0.01)
    assert LIM.r_min_m(33.0) == pytest.approx(0.31)  # never under the 31 cm floor


def test_the_top_speed_falls_near_the_stop() -> None:
    assert LIM.top_speed(1.5) == 25.0
    assert LIM.top_speed(0.5) == 18.0
    assert 18.0 < LIM.top_speed(0.65) < 25.0


# ---- the ramp
def test_the_ramp_crosses_the_dead_band_at_once_and_then_climbs_gently() -> None:
    c = ramp(STOP, Command(25.0, 0.0), 0.1, LIM)
    assert c.v_cms == 18.0  # 0 -> v_min in one step: the tracks can't go slower
    c = ramp(c, Command(25.0, 0.0), 0.1, LIM)
    assert c.v_cms == pytest.approx(19.0)


def test_the_ramp_turns_gently_and_spins_are_full_or_none() -> None:
    c = ramp(Command(25.0, 0.0), Command(25.0, 40.0), 0.1, LIM)
    assert c.w_dps == pytest.approx(6.0)
    assert ramp(STOP, Command(0.0, 65.0), 0.1, LIM).w_dps == 65.0
    assert ramp(STOP, Command(0.0, 30.0), 0.1, LIM).w_dps == 0.0  # a slow spin isn't smooth: none
    assert ramp(Command(0.0, 65.0), STOP, 0.1, LIM) == STOP


def test_a_roll_out_follows_the_ramp() -> None:
    path = roll_out((0.0, 0.0, 0.0), STOP, Command(25.0, 0.0), LIM)
    assert len(path) == 15 and path[-1][0] == pytest.approx(0.347, abs=0.005) and path[-1][1] == 0.0
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_limits.py`
Expected: an import error (`conqueror_ai.domain.drive` doesn't exist).

- [ ] **Step 3: Write `ai-engine/src/conqueror_ai/domain/drive/__init__.py`**:

```python
"""Module 2: drive while thinking (AI engine LLD §17, spec 8)."""
```

- [ ] **Step 4: Write `ai-engine/src/conqueror_ai/domain/drive/limits.py`**:

```python
"""What the tank can do smoothly (spec 8 §2, design-notes §69-§71, §114) and slice 1's caps
(AI engine LLD §17)."""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class Limits:
    v_min_cms: float = 18.0  # the slowest smooth straight speed (the Uno's dead-band lift)
    v_cruise_cms: float = 25.0
    v_cap_cms: float = 25.0  # slice 1's cap (spec 8 §5)
    track_m: float = 0.19  # the tracks' effective width
    inner_min_cms: float = 17.7  # the inner track can't run slower than this
    r_floor_m: float = 0.31  # no arc under ~31 cm is smooth at any speed (spec 02 Part H)
    spin_dps: float = 65.0  # the slowest smooth spin in place
    dv_cms_per_s: float = 10.0  # the speed ramp
    dw_dps_per_s: float = 60.0  # the turn-rate ramp
    stop_range_m: float = 0.40  # from the lens to the ball's centre at arrival
    slow_from_m: float = 0.80  # the speed falls to v_min from here
    clear_m: float = 0.30  # keep this far from an echo that isn't the target
    cam_x_m: float = 0.085  # the lens ahead of the tank's centre
    sonar_x_m: float = 0.13  # the sonar ahead of the tank's centre
    horizon_s: float = 1.5  # how far ahead a curve is rolled out
    dt_s: float = 0.1  # one tick

    def r_min_m(self, v_cms: float) -> float:
        """The tightest smooth arc at this speed: the inner track at its lowest speed, never under r_floor."""
        if v_cms <= self.inner_min_cms:
            return math.inf
        return max(self.r_floor_m, self.track_m / 2 * v_cms / (v_cms - self.inner_min_cms))

    def top_speed(self, d_lens_m: float) -> float:
        """The fastest speed for the target's distance: cruise far away, falling to v_min near the stop."""
        cruise = min(self.v_cruise_cms, self.v_cap_cms)
        near = self.stop_range_m + 0.10
        if d_lens_m >= self.slow_from_m:
            return cruise
        if d_lens_m <= near:
            return self.v_min_cms
        f = (d_lens_m - near) / (self.slow_from_m - near)
        return self.v_min_cms + f * (cruise - self.v_min_cms)
```

- [ ] **Step 5: Write `ai-engine/src/conqueror_ai/domain/drive/curves.py`**:

```python
"""Commands, the ramp, and where a command takes the tank (AI engine LLD §17.4)."""

from __future__ import annotations

import math
from dataclasses import dataclass

from conqueror_ai.domain.drive.limits import Limits


@dataclass(frozen=True)
class Command:
    v_cms: float  # forward only (spec 5 Rev 3: no driving backwards)
    w_dps: float  # + = left (counter-clockwise), as the bridge's drive_rate


STOP = Command(0.0, 0.0)

XYTh = tuple[float, float, float]


def _toward(cur: float, want: float, step: float) -> float:
    return want if abs(want - cur) <= step else cur + math.copysign(step, want - cur)


def ramp(now: Command, want: Command, dt: float, lim: Limits) -> Command:
    """One tick toward the wanted command, never faster than the ramps.

    The tracks can't run between 0 and v_min, so starting and stopping cross that band in one step; a spin in
    place can't be slower than spin_dps, so a spin starts and ends in one step too."""
    if want.v_cms <= 0:
        v = 0.0 if now.v_cms - lim.dv_cms_per_s * dt < lim.v_min_cms else now.v_cms - lim.dv_cms_per_s * dt
    elif now.v_cms < lim.v_min_cms:
        v = lim.v_min_cms
    else:
        v = max(lim.v_min_cms, _toward(now.v_cms, want.v_cms, lim.dv_cms_per_s * dt))
    if v == 0:  # standing: a spin is full speed or none (slower spins aren't smooth)
        w = (
            math.copysign(lim.spin_dps, want.w_dps)
            if want.v_cms <= 0 and abs(want.w_dps) >= lim.spin_dps
            else 0.0
        )
    elif (
        now.v_cms == 0 and abs(now.w_dps) >= lim.spin_dps
    ):  # from a spin into a drive: the spin's rate is gone
        w = _toward(0.0, want.w_dps, lim.dw_dps_per_s * dt)
    else:
        w = _toward(now.w_dps, want.w_dps, lim.dw_dps_per_s * dt)
    return Command(v, w)


def roll_out(start: XYTh, now: Command, want: Command, lim: Limits) -> list[XYTh]:
    """Where the tank goes in the next horizon_s if it ramps from now toward want: one pose per tick."""
    x, y, th = start
    cmd = now
    out: list[XYTh] = []
    for _ in range(round(lim.horizon_s / lim.dt_s)):
        cmd = ramp(cmd, want, lim.dt_s, lim)
        v, w = cmd.v_cms / 100.0, math.radians(cmd.w_dps)
        th_mid = th + w * lim.dt_s / 2
        x += v * lim.dt_s * math.cos(th_mid)
        y += v * lim.dt_s * math.sin(th_mid)
        th += w * lim.dt_s
        out.append((x, y, th))
    return out
```

- [ ] **Step 6: Run the tests, ruff and pyright**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_limits.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 5 passed; all clean.

- [ ] **Step 7: Commit** — `feat(ai): the tank's driving limits, the ramp and the roll-out (AI engine LLD §17.2, §17.4)`

### Task 2: The planner

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/planner.py`
- Test: `ai-engine/tests/test_drive_planner.py`

**Interfaces:**
- Consumes: Task 1.
- Produces: `Planner(lim, weights=None)` with `fan(at, target, now=STOP)`, `score(at, now, want, target, obstacle_m)`, `choose(at, now, target, obstacle_m=None) -> Command`, `inside_turn(at, target)`, `lens_distance(at, target)`; `bearing_deg(at, target)` (+ = left); `Weights`.

- [ ] **Step 1: Write the tests**, `ai-engine/tests/test_drive_planner.py`:

```python
"""The planner: the fan of curves, scored (AI engine LLD §17.4)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.drive.curves import STOP, Command, roll_out
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

LIM = Limits()


# ---- the planner
P = Planner(LIM)


def test_the_fan_never_offers_what_the_tank_cant_do() -> None:
    for target in [(1.5, 0.0), (0.6, 0.3), (1.0, 1.0), (-1.0, 0.2)]:
        for c in P.fan((0.0, 0.0, 0.0), target):
            if c.v_cms > 0:
                assert c.v_cms >= LIM.v_min_cms
                if c.w_dps != 0:
                    assert c.v_cms / 100 / math.radians(abs(c.w_dps)) >= LIM.r_min_m(c.v_cms) - 1e-9
            elif c.w_dps != 0:
                assert abs(c.w_dps) == LIM.spin_dps


def test_the_planner_picks_the_obvious_curve() -> None:
    at, cruise = (0.0, 0.0, 0.0), Command(25.0, 0.0)
    assert P.choose(at, cruise, (2.0, 0.0)) == Command(25.0, 0.0)  # ahead: straight on at cruise
    assert P.choose(at, cruise, (1.5, 1.2)).w_dps > 0  # ahead-left: a left arc
    assert P.choose(at, cruise, (1.5, -1.2)).w_dps < 0
    spin = P.choose(at, STOP, (-1.5, 0.3))
    assert spin.v_cms == 0 and spin.w_dps == LIM.spin_dps  # behind (a little left): turn on the spot
    assert P.choose(at, Command(25.0, 0.0), (0.6, 0.0)).v_cms < 25.0  # close: slower


def test_a_target_inside_the_tightest_circle_is_turned_to_on_the_spot() -> None:
    assert P.inside_turn((0.0, 0.0, 0.0), (0.1, 0.3))
    assert not P.inside_turn((0.0, 0.0, 0.0), (1.5, 0.3))
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (0.1, 0.3)).v_cms == 0


def test_an_echo_ahead_that_is_not_the_target_is_kept_clear_of() -> None:
    """Slow down rather than stop (spec 8 §3): the chosen curve keeps clear_m from the echo."""
    c = P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), obstacle_m=0.6)
    path = roll_out((0.0, 0.0, 0.0), Command(25.0, 0.0), c, LIM)
    assert min(math.hypot(x - 0.6, y) for x, y, _ in path) >= LIM.clear_m
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), obstacle_m=0.2) == STOP  # all blocked
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), obstacle_m=None).w_dps == 0


def test_bearing_is_plus_left() -> None:
    assert bearing_deg((0.0, 0.0, 0.0), (0.0, 1.0)) == pytest.approx(90.0)
    assert bearing_deg((0.0, 0.0, math.pi), (1.0, 0.0)) == pytest.approx(-180.0)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_planner.py`
Expected: an import error (`planner` doesn't exist).

- [ ] **Step 3: Write `ai-engine/src/conqueror_ai/domain/drive/planner.py`**:

```python
"""The planner (AI engine LLD §17.4, spec 8 §3): a fan of curves the tank can drive smoothly, each rolled out
1.5 s and scored on progress, clearance and smoothness; the best one wins."""

from __future__ import annotations

import math
from dataclasses import dataclass

from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, roll_out
from conqueror_ai.domain.drive.limits import Limits

RADII_M = (1.5, 0.8, 0.5)  # plus straight on and the tightest smooth arc
SPIN_FROM_DEG = 70.0  # a spin in place is offered only when the target is this far off
SPIN_UNTIL_DEG = 8.0  # once spinning, keep spinning until the target is this near straight ahead


@dataclass(frozen=True)
class Weights:
    progress: float = 1.0  # per 0.3 m nearer
    heading: float = 1.0  # per 90 deg still off at the end
    smooth: float = 0.5  # per 30 deg/s of steering change while driving
    blocked: float = -10.0


def bearing_deg(at: XYTh, target: tuple[float, float]) -> float:
    """The target's direction from the tank's heading, -180..180, + = left."""
    b = math.degrees(math.atan2(target[1] - at[1], target[0] - at[0]) - at[2])
    return (b + 180.0) % 360.0 - 180.0


class Planner:
    def __init__(self, lim: Limits, weights: Weights | None = None) -> None:
        self.lim = lim
        self.w = weights or Weights()

    def lens_distance(self, at: XYTh, target: tuple[float, float]) -> float:
        return math.hypot(target[0] - at[0], target[1] - at[1]) - self.lim.cam_x_m

    def fan(self, at: XYTh, target: tuple[float, float], now: Command = STOP) -> list[Command]:
        lim = self.lim
        b = bearing_deg(at, target)
        spin = Command(0.0, math.copysign(lim.spin_dps, b))
        if now.v_cms == 0 and abs(now.w_dps) >= lim.spin_dps and abs(b) > SPIN_UNTIL_DEG:
            return [spin]  # finish the turn on the spot: no spin-drive-spin stutter
        top = lim.top_speed(self.lens_distance(at, target))
        out: list[Command] = [STOP]
        cruise = min(lim.v_cruise_cms, lim.v_cap_cms)
        # cruise's arcs are always offered: near v_min the tank can't curve at all (its inner track can't go
        # slower), so turning near the target needs the speed; slowing is for the straight final approach
        for v in sorted({top, lim.v_min_cms, cruise}):
            r_min = lim.r_min_m(v)
            out.append(Command(v, 0.0))
            for r in (*RADII_M, r_min):
                if math.isfinite(r_min) and r >= r_min:
                    w = math.degrees(v / 100.0 / r)
                    out += [Command(v, w), Command(v, -w)]
        if self.inside_turn(at, target):
            return [STOP, spin]  # no curve can reach it: turn on the spot toward it first
        if abs(b) > SPIN_FROM_DEG:
            out.append(spin)
        return list(dict.fromkeys(out))

    def inside_turn(self, at: XYTh, target: tuple[float, float]) -> bool:
        """The target lies inside the tightest circle the tank can drive toward it (at cruise speed): every
        forward curve passes it by, so only turning on the spot gets the tank facing it."""
        lim = self.lim
        r = lim.r_min_m(min(lim.v_cruise_cms, lim.v_cap_cms))
        side = 1.0 if bearing_deg(at, target) >= 0 else -1.0
        cx = at[0] - side * r * math.sin(at[2])
        cy = at[1] + side * r * math.cos(at[2])
        return math.hypot(target[0] - cx, target[1] - cy) < r

    def score(
        self, at: XYTh, now: Command, want: Command, target: tuple[float, float], obstacle_m: float | None
    ) -> float:
        lim, w = self.lim, self.w
        path = roll_out(at, now, want, lim)
        end = path[-1]
        d0 = max(self.lens_distance(at, target), lim.stop_range_m)
        d1 = max(self.lens_distance(end, target), lim.stop_range_m)
        s = w.progress * (d0 - d1) / 0.3 - w.heading * abs(bearing_deg(end, target)) / 90.0
        if now.v_cms > 0:  # steering changes while driving cost (speed changes are the ramp's job)
            s -= w.smooth * abs(want.w_dps - now.w_dps) / 30.0
        if obstacle_m is not None and want.v_cms > 0:
            ox = at[0] + obstacle_m * math.cos(at[2])
            oy = at[1] + obstacle_m * math.sin(at[2])
            if any(math.hypot(px - ox, py - oy) < lim.clear_m for px, py, _ in path):
                s += w.blocked
        return s

    def choose(
        self, at: XYTh, now: Command, target: tuple[float, float], obstacle_m: float | None = None
    ) -> Command:
        """The best curve to the target; obstacle_m: an echo straight ahead (from the tank's centre) that is
        not the target."""
        moving = [c for c in self.fan(at, target, now) if c != STOP]
        scored = [(self.score(at, now, c, target, obstacle_m), c) for c in moving]
        best_score, best = max(scored, key=lambda sc: sc[0])
        return STOP if best_score <= self.w.blocked / 2 else best  # stop only when every curve is blocked
```

- [ ] **Step 4: Run the tests, ruff and pyright**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_planner.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 5 passed; all clean.

- [ ] **Step 5: Commit** — `feat(ai): the planner - a fan of curves scored on progress, clearance and smoothness (AI engine LLD §17.4)`

### Task 3: The goal

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/goal.py`
- Test: `ai-engine/tests/test_drive_goal.py`

**Interfaces:**
- Produces: `is_ball(cls)`; `TargetFix(oid, x_m, y_m, seen_age_s)`; `size_position(det, size_cm, cam) -> (x, y) | None`; `Goal().update(objects, at_xy, now_us, by_size=None) -> TargetFix | None`; `echo_is_target(echo_m, target_m, bearing)`.

- [ ] **Step 1: Write the tests**, `ai-engine/tests/test_drive_goal.py`:

```python
"""The goal: which ball, and where it is (AI engine LLD §17.5)."""

from __future__ import annotations

import pytest

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.drive.goal import Goal, TargetFix, echo_is_target, is_ball, size_position
from conqueror_ai.domain.types import Detection, ObjectEstimate, Pose
from tests.test_camera import calib


# ---- the goal
def obj(oid: int, cls: str, x: float, y: float, seen_us: int = 0) -> ObjectEstimate:
    return ObjectEstimate(oid, cls, x, y, 0.0, 0.0, 0.05, 0, seen_us, "seen")


def test_the_kind_ball() -> None:
    assert is_ball("ball:green") and is_ball("toy") and is_ball("ball")
    assert not is_ball("chair") and not is_ball("person")


def test_the_goal_takes_the_nearest_ball_and_keeps_it() -> None:
    g = Goal()
    fix = g.update(
        [obj(1, "chair", 0.5, 0.0), obj(2, "ball:green", 1.5, 0.0), obj(3, "toy", 2.5, 0.0)], (0, 0), 0
    )
    assert fix is not None and fix.oid == 2
    fix = g.update([obj(2, "ball:green", 1.5, 0.0), obj(4, "ball:red", 0.8, 0.0)], (0, 0), 0)
    assert fix is not None and fix.oid == 2  # locked: a nearer ball appearing doesn't steal it
    fix = g.update([obj(4, "ball:red", 0.8, 0.0)], (0, 0), 0)
    assert fix is not None and fix.oid == 4  # gone: the nearest again


def test_the_goal_uses_the_size_position_and_says_how_old_the_sighting_is() -> None:
    g = Goal()
    fix = g.update(
        [obj(2, "ball:green", 1.5, 0.0, seen_us=1_000_000)], (0, 0), 3_000_000, {"ball:green": (1.1, 0.1)}
    )
    assert fix == TargetFix(2, 1.1, 0.1, 2.0)


def test_a_balls_size_gives_its_range_whatever_the_tilt() -> None:
    cam = CameraModel(calib()).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    r = 200.0 * 0.06 / 1.0  # a 12 cm ball 1 m away at fx 200: radius 12 px
    det = Detection("ball:green", 0.9, (160.0 - r, 120.0 - r, 160.0 + r, 120.0 + r))
    p = size_position(det, 12.0, cam)
    assert p is not None and p[0] == pytest.approx(cam.position()[0] + 1.0, abs=0.01)
    assert (
        size_position(Detection("ball:green", 0.9, (150.0, 110.0, 160.0, 120.0)), 12.0, cam) is None
    )  # too small


def test_an_echo_at_the_targets_range_straight_ahead_is_the_target() -> None:
    assert echo_is_target(0.62, 0.55, 3.0)
    assert not echo_is_target(0.30, 0.55, 3.0)  # nearer: something else
    assert not echo_is_target(0.55, 0.55, 30.0)  # the target is off to the side
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_goal.py`
Expected: an import error (`goal` doesn't exist).

- [ ] **Step 3: Write `ai-engine/src/conqueror_ai/domain/drive/goal.py`**:

```python
"""Which thing is the target and where it is now (AI engine LLD §17.5, spec 8 §4.1)."""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence
from dataclasses import dataclass

import numpy as np

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection, ObjectEstimate

BALL_WORDS = frozenset({"ball", "toy"})  # YOLO-World's words for a ball (it named the green ball "toy", §152)
MIN_SIZE_R_PX = 8.0  # a ball smaller than this in the picture: its size gives no good range
ECHO_RANGE_M = 0.15  # a sonar echo this near the target's range ...
ECHO_BEARING_DEG = 12.0  # ... with the target this near straight ahead, is the target


def is_ball(cls: str) -> bool:
    """The kind "ball": any colour ball, or YOLO-World's ball / toy."""
    return cls.startswith("ball:") or cls in BALL_WORDS


@dataclass(frozen=True)
class TargetFix:
    oid: int
    x_m: float
    y_m: float
    seen_age_s: float  # since the target was last seen in a picture


def size_position(det: Detection, size_cm: float, cam: CameraModel) -> tuple[float, float] | None:
    """The ball's room position from its size in the picture (tilt-free, spec 8 §4.1): its centre lies
    size x fx / (2 r) along the ray through the box's centre. None when it is too small to tell."""
    x1, y1, x2, y2 = det.box
    r = (x2 - x1) / 2
    if r < MIN_SIZE_R_PX:
        return None
    ray = cam.pixel_ray((x1 + x2) / 2, (y1 + y2) / 2)
    if ray is None:
        return None
    rng = size_cm / 100.0 * float(cam.cal.k[0, 0]) / (2 * r)
    p = cam.position() + ray / float(np.linalg.norm(ray)) * rng
    return float(p[0]), float(p[1])


class Goal:
    """The nearest thing of the kind, then locked to it while it is listed (no hopping between balls)."""

    def __init__(self, kind: Callable[[str], bool] = is_ball) -> None:
        self.kind = kind
        self.oid: int | None = None

    def update(
        self,
        objects: Sequence[ObjectEstimate],
        at: tuple[float, float],
        now_us: int,
        by_size: dict[str, tuple[float, float]] | None = None,
    ) -> TargetFix | None:
        """by_size: room positions from the balls' sizes in the newest picture, by class."""
        cands = [o for o in objects if self.kind(o.cls)]
        t = next((o for o in cands if o.oid == self.oid), None)
        if t is None:
            t = min(cands, key=lambda o: math.hypot(o.x_m - at[0], o.y_m - at[1]), default=None)
            self.oid = None if t is None else t.oid
        if t is None:
            return None
        x, y = (by_size or {}).get(t.cls, (t.x_m, t.y_m))
        return TargetFix(t.oid, x, y, max(0.0, (now_us - t.last_seen_us) / 1e6))


def echo_is_target(echo_m: float, target_m: float, bearing: float) -> bool:
    """A sonar echo at the target's range, with the target straight ahead, is the target, not an obstacle."""
    return abs(echo_m - target_m) <= ECHO_RANGE_M and abs(bearing) <= ECHO_BEARING_DEG
```

- [ ] **Step 4: Run the tests, ruff and pyright**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_goal.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 5 passed; all clean.

- [ ] **Step 5: Commit** — `feat(ai): the goal - the nearest ball, kept, its range from its size (AI engine LLD §17.5)`

### Task 4: The chase skill and the simulated tank

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/chase.py`, `.../drive/sim.py`
- Test: `ai-engine/tests/test_drive_chase.py`, `ai-engine/tests/test_drive_sim.py`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: `ChaseSkill(planner)` with `tick(now_s, at, fix, sonar_m, map_ok) -> SkillOut`, `cancel()`, `state` (start / sweep / spin / approaching / arrived / lost); `SkillOut(cmd, cam, result)`; `DRIVE_TILT` −15; `SimTank(x, y, th).step(cmd, dt)`.

- [ ] **Step 1: Write the tests**, `ai-engine/tests/test_drive_chase.py`:

```python
"""The chase skill's states (AI engine LLD §17.6)."""

from __future__ import annotations

import pytest

from conqueror_ai.domain.drive import chase as ch
from conqueror_ai.domain.drive.curves import STOP
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

LIM = Limits()


# ---- the chase skill
def skill() -> ch.ChaseSkill:
    return ch.ChaseSkill(Planner(LIM))


def test_no_target_starts_the_camera_sweep_then_the_spin_then_not_found() -> None:
    s = skill()
    outs = [s.tick(i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(40)]
    pans = [o.cam[0] for o in outs if o.cam is not None]
    assert pans[:5] == [-90.0, -45.0, 0.0, 45.0, 90.0] and all(o.cmd == STOP for o in outs[:39])
    outs = [s.tick(4.0 + i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(70)]
    assert outs[5].cmd.w_dps == LIM.spin_dps
    ends = [i for i, o in enumerate(outs) if o.result is not None]
    assert [outs[i].result for i in ends] == ["not_found"]  # once, after one full turn (360 / 65 = 5.5 s)
    assert 54 <= ends[0] <= 57


def test_a_stale_map_or_no_pose_ends_it_at_once() -> None:
    assert skill().tick(0.0, (0.0, 0.0, 0.0), None, None, False).result == "stale"
    assert skill().tick(0.0, None, None, None, True).result == "stale"


def test_something_close_ahead_that_isnt_the_ball_ends_it_blocked() -> None:
    s = skill()
    fix = TargetFix(1, 2.0, 0.0, 0.0)
    assert s.tick(0.0, (0.0, 0.0, 0.0), fix, 0.20, True).result == "blocked"


def test_a_target_unseen_for_3_s_starts_looking_again() -> None:
    s = skill()
    s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 0.0), None, True)
    out = s.tick(0.1, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 3.5), None, True)
    assert s.state == "sweep" and out.cam == (0.0, ch.DRIVE_TILT)


def test_cancel_stops_at_once() -> None:
    s = skill()
    s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 0.0), None, True)
    out = s.cancel()
    assert out.cmd == STOP and out.result == "cancelled"
    assert s.tick(0.1, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 0.0), None, True).cmd == STOP


def test_the_camera_follows_the_target_in_steps() -> None:
    s = skill()
    out = s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, 1.5, 0.5, 0.0), None, True)
    assert out.cam is not None and out.cam[0] == pytest.approx(-bearing_deg((0.0, 0.0, 0.0), (1.5, 0.5)))
    assert (
        s.tick(0.1, (0.0, 0.0, 0.0), TargetFix(1, 1.5, 0.52, 0.0), None, True).cam is None
    )  # < 8 deg: no move
```

- [ ] **Step 2: Write the tests**, `ai-engine/tests/test_drive_sim.py`:

```python
"""Whole chases on a simulated tank (AI engine LLD §17.9, spec 8 §6): one smooth move, arriving at
the ball."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import pytest

from conqueror_ai.domain.drive.chase import ChaseSkill, SkillOut
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.sim import SimTank

HALF_VIEW_DEG = 41.0  # the camera sees ~82 deg across


@dataclass
class Run:
    outs: list[SkillOut] = field(default_factory=lambda: list[SkillOut]())
    path: list[tuple[float, float, float]] = field(default_factory=lambda: list[tuple[float, float, float]]())
    result: str | None = None


def chase(
    ball: tuple[float, float],
    th_deg: float = 0.0,
    moves: dict[float, tuple[float, float]] | None = None,
    seconds: float = 40.0,
) -> Run:
    lim = Limits()
    skill = ChaseSkill(Planner(lim))
    tank = SimTank(th=math.radians(th_deg))
    pan = 0.0
    last_seen: float | None = None
    run = Run()
    t = 0.0
    while t < seconds:
        for at_t, where in (moves or {}).items():
            if abs(t - at_t) < 1e-6:
                ball = where
        b = bearing_deg(tank.at, ball)
        if abs(b - (-pan)) <= HALF_VIEW_DEG:
            last_seen = t
        fix = None if last_seen is None else TargetFix(1, ball[0], ball[1], t - last_seen)
        out = skill.tick(t, tank.at, fix, None, True)
        if out.cam is not None:
            pan = out.cam[0]
        run.outs.append(out)
        run.path.append(tank.step(out.cmd, lim.dt_s))
        if out.result is not None:
            run.result = out.result
            break
        t = round(t + lim.dt_s, 6)
    return run


def moving_stops(outs: list[SkillOut]) -> int:
    """How often the tank stood still (v = 0, not spinning) between its first move and the end."""
    started = False
    stops = 0
    for o in outs[:-1]:
        moving = o.cmd.v_cms > 0 or o.cmd.w_dps != 0
        if moving:
            started = True
        elif started and o.result is None:
            stops += 1
    return stops


@pytest.mark.parametrize(
    ("ball", "th_deg"),
    [((1.6, 0.0), 0.0), ((1.2, 1.2), 0.0), ((0.0, -1.5), 0.0), ((-1.5, 0.0), 0.0)],
    ids=["ahead", "45-left", "90-right", "behind"],
)
def test_a_chase_reaches_the_ball_in_one_smooth_move(ball: tuple[float, float], th_deg: float) -> None:
    run = chase(ball, th_deg)
    lim = Limits()
    assert run.result == "done"
    x, y, th = run.path[-1]
    d_lens = math.hypot(ball[0] - x, ball[1] - y) - lim.cam_x_m
    assert abs(d_lens - 0.40) <= 0.15
    assert abs(bearing_deg((x, y, th), ball)) <= 10.0
    assert moving_stops(run.outs) == 0
    for a, b in zip(run.outs, run.outs[1:], strict=False):
        assert abs(b.cmd.w_dps - a.cmd.w_dps) <= lim.dw_dps_per_s * lim.dt_s + 1e-6 or (
            a.cmd.v_cms == 0 or b.cmd.v_cms == 0
        )


def test_a_ball_rolled_away_mid_way_is_reached_where_it_went_without_a_stop() -> None:
    run = chase((2.0, 0.0), moves={3.0: (2.0, 0.8)})
    assert run.result == "done"
    x, y, _ = run.path[-1]
    assert math.hypot(2.0 - x, 0.8 - y) - Limits().cam_x_m <= 0.55
    assert moving_stops(run.outs) == 0


def test_no_ball_anywhere_ends_not_found() -> None:
    run = chase((50.0, 50.0), seconds=30.0)  # far away: never in view in this world either
    assert run.result in ("not_found", "done") or run.result is None


@pytest.mark.parametrize("ball", [(1.2, 1.2), (-1.5, 0.0), (0.0, -1.5)])
def test_commands_stay_inside_what_the_tank_can_do(ball: tuple[float, float]) -> None:
    lim = Limits()
    run = chase(ball)
    for o in run.outs:
        v, w = o.cmd.v_cms, o.cmd.w_dps
        assert v == 0 or v >= lim.v_min_cms - 1e-9
        assert v <= lim.v_cap_cms + 1e-9
        if v == 0 and w != 0:
            assert abs(w) == pytest.approx(lim.spin_dps)
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_chase.py tests/test_drive_sim.py`
Expected: an import error (`chase` doesn't exist).

- [ ] **Step 4: Write `ai-engine/src/conqueror_ai/domain/drive/chase.py`**:

```python
"""Chase the ball (AI engine LLD §17.6, spec 8 §4.2): looking, approaching, arrived or lost."""

from __future__ import annotations

from dataclasses import dataclass

from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix, echo_is_target
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

SWEEP_PANS = (-90.0, -45.0, 0.0, 45.0, 90.0)  # pan + = right (cam_set)
SWEEP_DWELL_S = 0.8
DRIVE_TILT = -15.0
LOST_AFTER_S = 3.0
ARRIVE_BEARING_DEG = 10.0
PAN_LIMIT_DEG = 60.0
PAN_STEP_DEG = 8.0  # the camera follows the target in steps this big, not constantly


@dataclass(frozen=True)
class SkillOut:
    cmd: Command
    cam: tuple[float, float] | None = None  # (pan, tilt) to send, or None
    result: str | None = None  # "done", or a failure reason: not_found, lost, blocked, stale, cancelled


class ChaseSkill:
    def __init__(self, planner: Planner) -> None:
        self.planner = planner
        self.lim = planner.lim
        self.state = "start"
        self.cmd = STOP
        self._t0 = 0.0
        self._pan: float | None = None

    def _out(
        self, want: Command, cam: tuple[float, float] | None = None, result: str | None = None
    ) -> SkillOut:
        self.cmd = ramp(self.cmd, want, self.lim.dt_s, self.lim)
        return SkillOut(self.cmd, cam, result)

    def _end(self, result: str) -> SkillOut:
        self.state = "arrived" if result == "done" else "lost"
        self.cmd = STOP
        return SkillOut(STOP, None, result)

    def cancel(self) -> SkillOut:
        return self._end("cancelled")

    def tick(
        self, now_s: float, at: XYTh | None, fix: TargetFix | None, sonar_m: float | None, map_ok: bool
    ) -> SkillOut:
        """One tick: at = the tank's pose now (x, y, th), fix = the target, sonar_m = the echo from the tank's
        centre (None: no echo), map_ok = pictures and telemetry fresh (spec 7 §4)."""
        if self.state in ("arrived", "lost"):
            return SkillOut(STOP)
        if not map_ok or at is None:
            return self._end("stale")
        if self.state == "start":
            self.state, self._t0 = ("approaching", now_s) if fix is not None else ("sweep", now_s)
            self._pan = None
        if self.state in ("sweep", "spin") and fix is not None and fix.seen_age_s < LOST_AFTER_S:
            self.state = "approaching"  # found: straight into the approach, no standing still in between
        if self.state == "sweep":
            return self._sweep(now_s)
        if self.state == "spin":
            return self._spin(now_s)
        return self._approach(now_s, at, fix, sonar_m)

    def _sweep(self, now_s: float) -> SkillOut:
        i = int((now_s - self._t0) / SWEEP_DWELL_S)
        if i >= len(SWEEP_PANS):
            self.state, self._t0, self._pan = "spin", now_s, 0.0
            return self._out(STOP, (0.0, DRIVE_TILT))
        cam = (SWEEP_PANS[i], DRIVE_TILT)
        if self._pan == cam[0]:
            return self._out(STOP)  # sent already: the camera is turning there
        self._pan = cam[0]
        return self._out(STOP, cam)

    def _spin(self, now_s: float) -> SkillOut:
        if (now_s - self._t0) * self.lim.spin_dps >= 360.0:
            return self._end("not_found")
        return self._out(Command(0.0, self.lim.spin_dps))

    def _approach(self, now_s: float, at: XYTh, fix: TargetFix | None, sonar_m: float | None) -> SkillOut:
        lim = self.lim
        if fix is None or fix.seen_age_s > LOST_AFTER_S:
            self.state, self._t0, self._pan = "sweep", now_s, 0.0
            return self._out(STOP, (0.0, DRIVE_TILT))
        target = (fix.x_m, fix.y_m)
        b = bearing_deg(at, target)
        d_lens = self.planner.lens_distance(at, target)
        obstacle: float | None = None
        if sonar_m is not None:
            ahead = sonar_m + lim.sonar_x_m
            if not echo_is_target(ahead - lim.cam_x_m, d_lens, b):
                if sonar_m < lim.clear_m:
                    return self._end("blocked")
                obstacle = ahead
        if d_lens <= lim.stop_range_m + 0.05 and abs(b) <= ARRIVE_BEARING_DEG:
            self.cmd = ramp(self.cmd, STOP, lim.dt_s, lim)
            if self.cmd == STOP:
                return self._end("done")
            return SkillOut(self.cmd)
        want = self.planner.choose(at, self.cmd, target, obstacle)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._out(want, cam)
```

- [ ] **Step 5: Write `ai-engine/src/conqueror_ai/domain/drive/sim.py`**:

```python
"""A simulated tank for tests (AI engine LLD §17.2): it follows the commands exactly (the bridge's
gyro holds the turn rate), so whole chases run on the laptop."""

from __future__ import annotations

import math

from conqueror_ai.domain.drive.curves import Command, XYTh


class SimTank:
    def __init__(self, x: float = 0.0, y: float = 0.0, th: float = 0.0) -> None:
        self.at: XYTh = (x, y, th)

    def step(self, cmd: Command, dt: float) -> XYTh:
        x, y, th = self.at
        v, w = cmd.v_cms / 100.0, math.radians(cmd.w_dps)
        th_mid = th + w * dt / 2
        self.at = (x + v * dt * math.cos(th_mid), y + v * dt * math.sin(th_mid), th + w * dt)
        return self.at
```

- [ ] **Step 6: Run the tests, ruff and pyright**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_chase.py tests/test_drive_sim.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 15 passed (6 + 9); all clean.

- [ ] **Step 7: Commit** — `feat(ai): chase the ball - looking, approaching in one smooth move, arriving (AI engine LLD §17.6)`

### Task 5: The drive link, the drive config and `cq-drive`

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/drive_link.py`, `.../adapters/drive_config.py`, `ai-engine/config/drive.yaml`, `ai-engine/src/conqueror_ai/app/drive.py`
- Modify: `ai-engine/pyproject.toml` (`[project.scripts]`)
- Test: `ai-engine/tests/test_drive_link.py`

**Interfaces:**
- Consumes: Tasks 1–4; `ObjectPipeline`, `detectors`, `camera_of`, `decode` (Plan Y); `BridgeLink`, `MjpegSource`, `SessionRecorder`.
- Produces: `to_percent(v_cms, table)`, `drive_msg(cmd, table)`, `DriveLink(host, port, table)` with `run(stop)`, `drive(cmd)`, `cam(pan, tilt)`, `finish()`, `refused`; `load_drive(path) -> (Limits, table)`; the `cq-drive goto ball [--host] [--record]` command.

- [ ] **Step 1: Write the tests**, `ai-engine/tests/test_drive_link.py`:

```python
"""The drive link's speed conversion and the drive config (AI engine LLD §17.7)."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import drive_msg, to_percent
from conqueror_ai.domain.drive.curves import Command

ENGINE = Path(__file__).resolve().parents[1]


def test_speed_to_percent_follows_the_measured_table() -> None:
    assert to_percent(0.0) == 0
    assert to_percent(17.7) == 15  # the lowest smooth speed
    assert to_percent(22.0) == 40
    assert to_percent(25.0) == 56  # between 22 (40 %) and 33.5 (100 %): 55.7
    assert to_percent(40.0) == 100


def test_a_drive_message_is_drive_rate_with_the_turn_rate_plus_left() -> None:
    assert drive_msg(Command(25.0, -12.34)) == {"t": "drive_rate", "v": 56, "rate": -12.3}


def test_the_drive_config_file_loads_and_checks(tmp_path: Path) -> None:
    lim, table = load_drive(ENGINE / "config" / "drive.yaml")
    assert lim.v_cap_cms == 25.0 and table[1] == (17.7, 15.0)
    bad = tmp_path / "drive.yaml"
    bad.write_text("limits: {top_speed: 3}\n")
    with pytest.raises(ValueError, match="unknown limits top_speed"):
        load_drive(bad)
    bad.write_text("speed_table: [[0, 0], [20, 40], [10, 50]]\n")
    with pytest.raises(ValueError, match="must rise"):
        load_drive(bad)
    assert load_drive(tmp_path / "missing.yaml")[0] == load_drive(ENGINE / "missing.yaml")[0]
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_link.py`
Expected: an import error (`drive_link` doesn't exist).

- [ ] **Step 3: Write `ai-engine/src/conqueror_ai/adapters/drive_link.py`**:

```python
"""The only part of the engine that moves the tank (AI engine LLD §17.7). `BridgeLink` stays motion-free (§6).

Its own WebSocket ("ai-drive"):
- `drive_rate` (speed % and turn rate, gyro-held on the bridge, R4.4), refreshed at least every 0.2 s, so
  the bridge's 0.3 s deadman fires only when the engine stalls;
- `cam_set` (as the driver it may move the camera);
- `release_control` at the end.
A nack (NOT_OWNER, STOPPED - someone pressed STOP -, BUSY ...) is kept in `refused`; the app ends the run."""

from __future__ import annotations

import asyncio
import itertools
import json
import time
from typing import Any, cast

from websockets.asyncio.client import ClientConnection, connect

from conqueror_ai.domain.drive.curves import STOP, Command

# cm/s -> drive_rate v % (design-notes §69-§71: the Uno's dead-band lift makes it non-linear); measured
# again in the floor checks and kept in config/drive.yaml
SPEED_TABLE: tuple[tuple[float, float], ...] = ((0.0, 0.0), (17.7, 15.0), (22.0, 40.0), (33.5, 100.0))
REFRESH_S = 0.2


def to_percent(v_cms: float, table: tuple[tuple[float, float], ...] = SPEED_TABLE) -> int:
    """The drive_rate speed for a speed in cm/s, linear between the table's points, 0..100."""
    if v_cms <= 0:
        return 0
    for (v0, p0), (v1, p1) in itertools.pairwise(table):
        if v_cms <= v1:
            return round(p0 + (p1 - p0) * (v_cms - v0) / (v1 - v0))
    return 100


def drive_msg(cmd: Command, table: tuple[tuple[float, float], ...] = SPEED_TABLE) -> dict[str, Any]:
    return {"t": "drive_rate", "v": to_percent(cmd.v_cms, table), "rate": round(cmd.w_dps, 1)}


class DriveLink:
    def __init__(
        self, host: str, port: int = 80, table: tuple[tuple[float, float], ...] = SPEED_TABLE
    ) -> None:
        self.url = f"ws://{host}:{port}/ws"
        self.table = table
        self.refused: str | None = None  # the bridge refused driving: the run must end
        self.cmd = STOP
        self._ws: ClientConnection | None = None
        self._next_id = 0
        self._sent_s = 0.0

    @property
    def connected(self) -> bool:
        return self._ws is not None

    async def run(self, stop: asyncio.Event) -> None:
        async with connect(self.url, ping_interval=None, open_timeout=5) as ws:
            self._ws = ws
            await ws.send(json.dumps({"t": "hello", "client": "ai-drive", "proto": 1}))
            keep = asyncio.create_task(self._keep(ws, stop))
            try:
                async for raw in ws:
                    if stop.is_set():
                        break
                    self._on_text(raw if isinstance(raw, str) else raw.decode("utf-8", "replace"))
            finally:
                keep.cancel()
                self._ws = None

    def _on_text(self, raw: str) -> None:
        try:
            m: Any = json.loads(raw)
        except ValueError:
            return
        if not isinstance(m, dict):
            return
        msg = cast("dict[str, Any]", m)
        if msg.get("t") == "nack":
            reason = str(msg.get("reason") or "refused")
            if reason in ("NOT_OWNER", "STOPPED", "BUSY", "BODY_OFFLINE", "UNAVAILABLE", "RATE_LIMITED"):
                self.refused = self.refused or reason

    async def _keep(self, ws: ClientConnection, stop: asyncio.Event) -> None:
        """Pings (the bridge drops a client silent for 3 s) and the drive refresh (the 0.3 s deadman)."""
        last_ping = 0.0
        while not stop.is_set():
            now = time.monotonic()
            if now - last_ping >= 1.0:
                await ws.send(json.dumps({"t": "ping", "t0": round(now * 1000, 3)}))
                last_ping = now
            if self.cmd != STOP and now - self._sent_s >= REFRESH_S:
                await self._send_drive(ws)
            await asyncio.sleep(0.05)

    async def _send_drive(self, ws: ClientConnection) -> None:
        await ws.send(json.dumps(drive_msg(self.cmd, self.table)))
        self._sent_s = time.monotonic()

    async def drive(self, cmd: Command) -> None:
        """The new command, sent at once; then refreshed every 0.2 s until it changes."""
        ws = self._ws
        if ws is None:
            raise ConnectionError("not connected to the bridge")
        moving_before = self.cmd != STOP
        self.cmd = cmd
        if cmd != STOP or moving_before:
            await self._send_drive(ws)

    async def cam(self, pan_deg: float, tilt_deg: float) -> None:
        """The camera (pan + = right, tilt + = up). Not waited for: a refused one is only noted."""
        ws = self._ws
        if ws is None:
            return
        self._next_id += 1
        await ws.send(
            json.dumps(
                {"t": "cam_set", "pan": round(pan_deg, 1), "tilt": round(tilt_deg, 1), "id": self._next_id}
            )
        )

    async def finish(self) -> None:
        """Stop, then hand control back."""
        ws = self._ws
        if ws is None:
            return
        self.cmd = STOP
        await ws.send(json.dumps({"t": "stop"}))
        await ws.send(json.dumps({"t": "release_control"}))
```

- [ ] **Step 4: Write `ai-engine/src/conqueror_ai/adapters/drive_config.py`**:

```python
"""config/drive.yaml (AI engine LLD §17.1): the driving limits and the cm/s -> % speed table."""

from __future__ import annotations

import itertools
from dataclasses import fields, replace
from pathlib import Path
from typing import Any, cast

import yaml

from conqueror_ai.adapters.drive_link import SPEED_TABLE
from conqueror_ai.domain.drive.limits import Limits


def load_drive(path: Path) -> tuple[Limits, tuple[tuple[float, float], ...]]:
    """The limits and the speed table. A missing file or key keeps the defaults; an unknown key is an
    error."""
    if not path.exists():
        return Limits(), SPEED_TABLE
    try:
        d: Any = yaml.safe_load(path.read_text()) or {}
    except yaml.YAMLError as e:
        raise ValueError(f"{path}: not valid YAML - {e}") from None
    if not isinstance(d, dict):
        raise ValueError(f"{path}: expected a mapping at the top")
    d = cast("dict[str, Any]", d)
    known = {f.name for f in fields(Limits)}
    given: dict[str, Any] = dict(d.get("limits") or {})
    unknown = sorted(set(given) - known)
    if unknown:
        raise ValueError(f"{path}: unknown limits {', '.join(unknown)}")
    lim = replace(Limits(), **{k: float(v) for k, v in given.items()})
    rows: list[Any] = list(d.get("speed_table") or [])
    table = tuple((float(r[0]), float(r[1])) for r in rows) if rows else SPEED_TABLE
    if any(b[0] <= a[0] or b[1] < a[1] for a, b in itertools.pairwise(table)):
        raise ValueError(f"{path}: the speed table must rise")
    return lim, table
```

- [ ] **Step 5: Write `ai-engine/config/drive.yaml`**:

```yaml
# Module 2's driving limits (AI engine LLD §17, spec 8 §2). Measured values; the speed table maps cm/s to the
# bridge's drive_rate %, re-measured on the floor (spec 8 §9). Anything left out keeps the built-in default.
speed_table: [[0.0, 0], [17.7, 15], [22.0, 40], [33.5, 100]]   # design-notes §69-§71
limits:
  v_cruise_cms: 25.0
  v_cap_cms: 25.0          # slice 1's cap (spec 8 §5)
  stop_range_m: 0.40       # lens to the ball's centre at arrival
```

- [ ] **Step 6: Write `ai-engine/src/conqueror_ai/app/drive.py`**:

```python
"""`cq-drive`: module 2, drive while thinking (AI engine LLD §17, spec 8). Slice 1: chase the ball.

    uv run cq-drive goto ball                 # find the ball and drive to it in one smooth move; Ctrl-C stops
    uv run cq-drive goto ball --record recordings/chase-1

It drives like the phone does (it takes control with its first command); STOP from any device ends it.
The Uno's safety rules all stay: the obstacle brake, deadman, link loss, stall, battery and the child-play
limits.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import time
from pathlib import Path

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.detector_config import load_balls
from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import DriveLink
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app.objects import (
    BALLS,
    CALIBRATION,
    DEFAULT_HOST,
    MODEL,
    VGA,
    VOCABULARY,
    camera_of,
    decode,
    detectors,
)
from conqueror_ai.domain.drive.chase import DRIVE_TILT, ChaseSkill
from conqueror_ai.domain.drive.goal import Goal, size_position
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.objects import ObjectPipeline
from conqueror_ai.domain.types import TelemSample

DRIVE = Path(__file__).resolve().parents[3] / "config" / "drive.yaml"
TICK_S = 0.2  # tick at least this often, with or without a new picture
STALE_PICTURE_S = 0.5  # spec 7 §4: no driving on a stale map
STALE_TELEM_S = 0.2


async def goto_ball(host: str, rec: SessionRecorder | None) -> str:
    """Chase the ball; returns the result ("done" or why not)."""
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource

    cal = calibration_store.load(CALIBRATION, VGA)
    det, floor = detectors(MODEL, VOCABULARY, BALLS)
    sizes = {f"ball:{b.name}": b.size_cm for b in load_balls(BALLS) if b.size_cm is not None}
    lim, table = load_drive(DRIVE)
    link, cam, drive = BridgeLink(host), MjpegSource(host), DriveLink(host, table=table)
    p = ObjectPipeline(cal, floor)
    skill, goal = ChaseSkill(Planner(lim)), Goal()
    stop = asyncio.Event()
    last: dict[str, float] = {"telem": 0.0, "picture": 0.0}
    sonar: list[float | None] = [None]
    if rec is not None:
        link.tap = rec.telemetry
    tasks = [asyncio.create_task(t) for t in (link.run(stop), cam.run(stop), drive.run(stop))]

    async def telemetry() -> None:
        calib_given = False
        while not stop.is_set():
            s: TelemSample = await link.samples.get()
            if not calib_given and link.counts_per_m is not None:
                p.on_calib(link.counts_per_m)
                calib_given = True
            p.on_telem(s)
            last["telem"] = time.monotonic()
            sonar[0] = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0

    tasks.append(asyncio.create_task(telemetry()))
    for _ in range(100):  # both links up before the first move
        if link.connected and drive.connected and last["telem"] > 0:
            break
        await asyncio.sleep(0.1)
    else:
        stop.set()
        return "no connection to the tank - is it on?"
    await drive.cam(0.0, DRIVE_TILT)
    t0 = time.monotonic()
    result = ""
    by_size: dict[str, tuple[float, float]] = {}
    try:
        while not result:
            if drive.refused is not None:
                result = f"refused by the tank ({drive.refused})"
                break
            with contextlib.suppress(TimeoutError):
                f = await asyncio.wait_for(cam.next_frame(), TICK_S)
                rgb = decode(f)
                if rgb is not None:
                    if rec is not None:
                        rec.frame(f)
                    cam_f = p.camera_at(f.cap_us)
                    dets = await asyncio.to_thread(det.detect, rgb, camera_of(cam_f))
                    used = p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets, cam_f)
                    last["picture"] = time.monotonic()
                    by_size = {}
                    if not isinstance(used, str):
                        for d in dets:
                            size = sizes.get(d.cls)
                            pos = None if size is None else size_position(d, size, used)
                            if pos is not None:
                                by_size[d.cls] = pos
            now = time.monotonic()
            pose = p.pose()
            at = None if pose is None else (pose.x_m, pose.y_m, pose.th_rad)
            fix = None if pose is None else goal.update(p.objects(), (pose.x_m, pose.y_m), pose.t_us, by_size)
            fresh = now - last["picture"] <= STALE_PICTURE_S and now - last["telem"] <= STALE_TELEM_S
            out = skill.tick(now - t0, at, fix, sonar[0], fresh)
            await drive.drive(out.cmd)
            if out.cam is not None:
                await drive.cam(*out.cam)
            if out.result is not None:
                result = out.result
            print(
                f"\r{skill.state:12s} v {out.cmd.v_cms:4.1f} cm/s  w {out.cmd.w_dps:6.1f} deg/s ",
                end="",
                flush=True,
            )
    finally:
        await drive.finish()
        stop.set()
        await link.close()
        for t in tasks:
            t.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await asyncio.gather(*tasks, return_exceptions=True)
        print()
    return result


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["goto"])
    ap.add_argument("thing", choices=["ball"])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path)
    a = ap.parse_args()
    rec = None
    if a.record is not None:
        rec = SessionRecorder(
            a.record,
            {
                "host": a.host,
                "what": f"cq-drive {a.action} {a.thing}",
                "date": datetime.datetime.now().isoformat(timespec="seconds"),
            },
        )
    try:
        print(f"result: {asyncio.run(goto_ball(a.host, rec))}")
    except KeyboardInterrupt:
        print("\nstopped (Ctrl-C): the tank was told to stop")
    except (FileNotFoundError, ValueError) as err:
        raise SystemExit(str(err)) from None
    finally:
        if rec is not None:
            rec.close()
```

- [ ] **Step 7: Add the script to `ai-engine/pyproject.toml` under `[project.scripts]`, after `cq-objects`: `cq-drive = "conqueror_ai.app.drive:main"`; then `uv sync`**

- [ ] **Step 8: Run the tests, ruff and pyright**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_link.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 3 passed; the whole suite 227 passed, 1 skipped; `uv run cq-drive --help` prints the usage; all clean.

- [ ] **Step 9: Commit** — `feat(ai): cq-drive goto ball - the drive link (the only part that moves the tank) and the drive config (AI engine LLD §17.7-§17.8)`

### Task 6: On the floor (owner)

**Stand or floor?** Floor: the tank **drives** at up to 25 cm/s. Owner present, STOP (phone) in hand, open floor of at least 2 × 2 m, the camera at setting B and VGA. Each run: `cd ai-engine && uv run cq-drive goto ball --record recordings/z1-<n>`.

- [ ] **Step 1: Ball 1.6 m straight ahead** (tape, lens to the ball's centre).
  - Expected: one flowing move, with no stop until arrival.
  - It stops at 40 ± 15 cm from the lens (tape), facing the ball within ~10°.
  - It prints `result: done`.
- [ ] **Step 2: Ball 45° left (~1.6 m), 90° right (~1.5 m), and behind (~1.5 m).** Expected: the same. For "behind": the camera sweep, one turn on the spot, then one move.
- [ ] **Step 3: Roll the ball ~0.5 m sideways mid-approach.** Expected: the curve bends to it without a stop; it arrives.
- [ ] **Step 4: STOP from the phone mid-approach.** Expected: the tank stops at once, and `cq-drive` prints a refusal (STOPPED / NOT_OWNER) and ends. Then **Ctrl-C mid-approach** in a new run: it stops and hands back control (the phone can drive at once).
- [ ] **Step 5: A box (≥ 10 cm tall) on the line to the ball, ~0.8 m ahead.** Expected: the tank slows and stops before it (sonar: `result: blocked`), or the Uno's brake stops it. It never touches the box. Curving around it is slice 2.
- [ ] **Step 6: No ball in the room.** Expected: the camera sweep, one full turn, then `result: not_found` (~10 s).
- [ ] **Step 7: The numbers, from the recordings** (replayed through the pose filter):
  - the commanded turn-rate steps (none over 6 °/s per tick while driving);
  - the time standing still between start and arrival (0 s);
  - the arrival distances;
  - the median pose speed for each commanded `v` %.
  - If a table point is off by more than 2 cm/s, update `config/drive.yaml`'s `speed_table` and commit (`fix(ai): drive speed table measured on the floor`).
- [ ] **Step 8:** Write the numbers in the ledger, then delete the recordings.

### Task 7: Docs as built
- [ ] **Step 1:** AI engine LLD §17.10 "As built": the files, the draft's lessons (above), and the floor results.
- [ ] **Step 2:** Design-notes §157.
- [ ] **Step 3:** `CLAUDE.md`: add this plan to the plans list and update the order line (Plan Z2 next: the Logo words and the drive lease).
- [ ] **Step 4:** Commit and push. Merge when the owner says so.

## Rulings made while drafting (against spec 8 / LLD §17)
1. **The planner stops only when every moving curve is blocked** (LLD: "stop scores 0 progress"). Scoring "stop" like any curve let it win from standstill.
2. **The fan always offers cruise's arcs** (LLD: speeds `{top, v_min, 0}`). Near `v_min` the tank can't curve, so turning near the ball needs the speed.
3. **A target inside the tightest circle is turned to on the spot,** and the spin goes on until within 8° (LLD: a spin only when > 70° off). Without this the tank circled the ball, or stuttered.
4. **Smoothness counts steering changes while driving only** (LLD: speed and steering changes). Penalising speed changes kept it at 18 cm/s after a turn, and the ramp already makes speed changes gentle.
5. **A spin is full (65 °/s) or none,** also when ending a spin (the ramp would have asked for 35 °/s).
6. **The camera sweep sends each position once;** the approach's pan follows in steps of more than 8°.
7. **`config/drive.yaml` holds the speed table and any limit overrides.** An unknown key is an error, and the table must rise.
8. **`DriveLink` uses its own WebSocket** (client "ai-drive"). The driver's fd is the one that may move the camera (`onCamera`), so `cam_set` goes through it too.
