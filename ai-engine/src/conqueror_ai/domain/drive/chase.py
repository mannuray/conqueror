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
