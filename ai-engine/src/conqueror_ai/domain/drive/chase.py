"""Chase the ball (AI engine LLD §17.6, spec 8 §4.2): looking, approaching, arrived or lost."""

from __future__ import annotations

import math
from dataclasses import dataclass

from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix, echo_is_target
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

SWEEP_PANS = (0.0, -45.0, 45.0, -90.0, 90.0)  # pan + = right (cam_set); straight ahead first
SWEEP_LOOK_S = 0.8  # still pictures at each position (the map skips pictures while the camera moves)
SWEEP_MAX_S = 2.5  # at most this long at one position (a camera that never settles)
DRIVE_TILT = -15.0
LOST_AFTER_S = 3.0
ARRIVE_BEARING_DEG = 10.0
PAN_LIMIT_DEG = 60.0
PAN_STEP_DEG = 8.0  # the camera follows the target in steps this big, not constantly
SEARCH_TURN_DEG = 180.0  # after the first sweep: half a turn on the spot, then sweep what was behind
MAX_TICK_S = 0.3  # a longer gap between ticks counts as this (no jump in the ramps after a stall)
STALE_GIVE_UP_S = 2.0  # a stale map stops the tank at once; this long ends the run (design-notes §157.2)


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
        self._last_s: float | None = None
        self._dt = planner.lim.dt_s  # the real time since the last tick: the loop ticks once per picture
        self._swept = 0
        self._i = 0  # the sweep's position
        self._pos_t0 = 0.0  # when the camera was sent there
        self._look_t0: float | None = None  # since when its pictures are still
        self._cam_still = True
        self._stale_t0: float | None = None  # since when the map is stale

    def _out(
        self, want: Command, cam: tuple[float, float] | None = None, result: str | None = None
    ) -> SkillOut:
        self.cmd = ramp(self.cmd, want, self._dt, self.lim)
        return SkillOut(self.cmd, cam, result)

    def _end(self, result: str) -> SkillOut:
        self.state = "arrived" if result == "done" else "lost"
        self.cmd = STOP
        return SkillOut(STOP, None, result)

    def cancel(self) -> SkillOut:
        return self._end("cancelled")

    def tick(
        self,
        now_s: float,
        at: XYTh | None,
        fix: TargetFix | None,
        sonar_m: float | None,
        map_ok: bool,
        cam_still: bool = True,
    ) -> SkillOut:
        """One tick: at = the tank's pose now (x, y, th), fix = the target, sonar_m = the echo's distance from
        the sonar at the tank's front (None: no echo), map_ok = pictures and telemetry fresh (spec 7 §4),
        cam_still = the newest picture wasn't skipped for the camera moving."""
        if self.state in ("arrived", "lost"):
            return SkillOut(STOP)
        if self._last_s is not None:
            self._dt = min(max(now_s - self._last_s, 0.0), MAX_TICK_S)
        self._last_s = now_s
        self._cam_still = cam_still
        if not map_ok or at is None:  # it can't see: stop at once (no ramp), drive on when the map is fresh
            if self._stale_t0 is None:
                self._stale_t0 = now_s
            if now_s - self._stale_t0 >= STALE_GIVE_UP_S - 1e-9:
                return self._end("stale")
            self.cmd = STOP
            return SkillOut(STOP)
        self._stale_t0 = None
        if self.state == "start":
            self.state, self._t0 = ("approaching", now_s) if fix is not None else ("sweep", now_s)
            self._pan, self._i = None, 0
        if self.state in ("sweep", "spin") and fix is not None and fix.seen_age_s < LOST_AFTER_S:
            self.state = "approaching"  # found: straight into the approach, no standing still in between
        if self.state == "sweep":
            return self._sweep(now_s)
        if self.state == "spin":
            return self._spin(now_s)
        return self._approach(now_s, at, fix, sonar_m)

    def _sweep(self, now_s: float) -> SkillOut:
        """The camera to each pan in turn; at each, SWEEP_LOOK_S of still pictures (SWEEP_MAX_S at most)."""
        if self._i >= len(SWEEP_PANS):
            if self._swept >= 1:
                return self._end("not_found")
            self._swept += 1
            self.state, self._t0, self._pan = "spin", now_s, 0.0
            return self._out(STOP, (0.0, DRIVE_TILT))
        pan = SWEEP_PANS[self._i]
        if self._pan != pan:
            self._pan, self._pos_t0, self._look_t0 = pan, now_s, None
            return self._out(STOP, (pan, DRIVE_TILT))
        if not self._cam_still:
            self._look_t0 = None
        elif self._look_t0 is None:
            self._look_t0 = now_s
        looked = self._look_t0 is not None and now_s - self._look_t0 >= SWEEP_LOOK_S - 1e-9
        if looked or now_s - self._pos_t0 >= SWEEP_MAX_S - 1e-9:
            self._i += 1
            return self._sweep(now_s)
        return self._out(STOP)

    def _spin(self, now_s: float) -> SkillOut:
        """Half a turn on the spot. The map can't use pictures taken while spinning (faster than its 45 deg/s
        gate), so the tank then stands and sweeps the camera over what was behind it."""
        if (now_s - self._t0) * self.lim.spin_dps >= SEARCH_TURN_DEG:
            self.state, self._t0, self._pan, self._i = "sweep", now_s, None, 0
            return self._sweep(now_s)
        return self._out(Command(0.0, self.lim.spin_dps))

    def _approach(self, now_s: float, at: XYTh, fix: TargetFix | None, sonar_m: float | None) -> SkillOut:
        lim = self.lim
        if fix is None or fix.seen_age_s > LOST_AFTER_S:
            self.state, self._t0, self._pan, self._i = "sweep", now_s, None, 0
            return self._sweep(now_s)
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
        if d_lens <= lim.stop_range_m + 0.05:
            if abs(b) <= ARRIVE_BEARING_DEG:
                self.cmd = ramp(self.cmd, STOP, self._dt, lim)
                if self.cmd == STOP:
                    return self._end("done")
                return SkillOut(self.cmd)
            # close enough but off to the side: driving on would pass it, so turn on the spot to face it
            return self._out(Command(0.0, math.copysign(lim.spin_dps, b)))
        want = self.planner.choose(at, self.cmd, target, obstacle)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._out(want, cam)
