"""Chase the ball (AI engine LLD §17.6, spec 8 §4.2): looking, approaching, arrived or lost."""

from __future__ import annotations

import math
from dataclasses import dataclass, replace

from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix, echo_is_target
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.recovery import Recovery
from conqueror_ai.domain.drive.route import RoutePlanner, aim_point
from conqueror_ai.domain.room_map import MapSnapshot

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
BLOCKED_GIVE_UP_S = 3.0  # without the map: standing with every curve blocked this long ends the run
MOVED_M = 0.05  # on the map: moving this far ...
TURNED_DEG = 15.0  # ... or turning this far is progress; none for stuck_s starts the ways out (Plan Z6)
STALE_GIVE_UP_S = 2.0  # a stale map stops the tank at once; this long ends the run (design-notes §157.2)
TARGET_R_M = 0.12  # on the room map, the target's own marks (the ball is 12 cm) don't count as an obstacle
SLOW_CMS = 18.0  # spec 9 §5: unsure of the camera angle, or no depth


@dataclass(frozen=True)
class SkillOut:
    cmd: Command
    cam: tuple[float, float] | None = None  # (pan, tilt) to send, or None
    result: str | None = None  # "done", or a failure reason: not_found, lost, blocked, stale, cancelled


class ChaseSkill:
    def __init__(self, planner: Planner, cost: CostSettings | None = None) -> None:
        self.planner = planner
        self.settings = cost or CostSettings()
        self.slow = Planner(replace(planner.lim, v_cap_cms=min(planner.lim.v_cap_cms, SLOW_CMS)), planner.w)
        self.lim = planner.lim
        self.routes = RoutePlanner(self.lim)
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
        self._blocked_t0: float | None = None  # since when every curve is blocked
        self.recovery = Recovery(planner.lim, planner.foot, DRIVE_TILT)  # on the map (Plan Z6)
        self._recovering = False
        self._progress: tuple[XYTh, float] | None = None  # where and when it last got somewhere (on the map)
        self._spin_turned = 0.0  # the search turn, measured from the pose
        self._spin_th: float | None = None

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
        snap: MapSnapshot | None = None,
    ) -> SkillOut:
        """One tick: at = the tank's pose now (x, y, th), fix = the target, sonar_m = the echo's distance from
        the sonar at the tank's front (None: no echo), map_ok = pictures and telemetry fresh (spec 7 §4),
        cam_still = the newest picture wasn't skipped for the camera moving, snap = the room map (Plan Z5:
        the approach goes round obstacles; None: slice 1's sonar-only approach)."""
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
            return self._spin(now_s, at, snap)
        return self._approach(now_s, at, fix, sonar_m, snap)

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

    def _spin(self, now_s: float, at: XYTh, snap: MapSnapshot | None) -> SkillOut:
        """Half a turn on the spot, measured from the pose. The map can't use pictures taken while spinning
        (faster than its 45 deg/s gate), so the tank then stands and sweeps the camera over what was behind
        it. On the map the turn is checked like every move (review C1): refused, the search sweeps from
        here."""
        if self._spin_th is not None:
            self._spin_turned += abs((math.degrees(at[2] - self._spin_th) + 180.0) % 360.0 - 180.0)
        self._spin_th = at[2]
        rest = SEARCH_TURN_DEG - self._spin_turned
        refused = snap is not None and not self.planner.can_spin(
            at, CostMap(snap, self.settings), True, max(rest, 1.0)
        )
        if rest <= 1e-9 or refused:
            self.state, self._t0, self._pan, self._i = "sweep", now_s, None, 0
            self._spin_turned, self._spin_th = 0.0, None
            self.cmd = STOP
            return self._sweep(now_s)
        return self._out(Command(0.0, self.lim.spin_dps))

    def _approach(
        self, now_s: float, at: XYTh, fix: TargetFix | None, sonar_m: float | None, snap: MapSnapshot | None
    ) -> SkillOut:
        lim = self.lim
        if fix is None or fix.seen_age_s > LOST_AFTER_S:
            self.state, self._t0, self._pan, self._i = "sweep", now_s, None, 0
            self._recovering, self._blocked_t0, self._progress = False, None, None  # review I2
            self.cmd = STOP
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
        fan = (
            None if snap is None else CostMap(snap, self.settings, ignore=(target[0], target[1], TARGET_R_M))
        )
        if fan is not None and self._recovering:  # stuck on the map: the standard ways out (Plan Z6)
            return self._recover(now_s, at, fan)
        if d_lens <= lim.stop_range_m + 0.05:
            if abs(b) <= ARRIVE_BEARING_DEG:
                self.cmd = ramp(self.cmd, STOP, self._dt, lim)
                if self.cmd == STOP:
                    return self._end("done")
                return SkillOut(self.cmd)
            if not self.planner.can_spin(
                at, fan, b > 0, abs(b)
            ):  # turning to face it would scrape (review C1)
                self.cmd = STOP
                return self._end("done")
            # close enough but off to the side: driving on would pass it, so turn on the spot to face it
            return self._out(Command(0.0, math.copysign(lim.spin_dps, b)))
        if snap is None:
            want = self.planner.choose(at, self.cmd, target, obstacle)
        else:  # round obstacles on the room map: steer along the route, slow down for the ball itself
            planner = self.planner if snap.angle_trusted and snap.depth_ok else self.slow
            clear = fan or CostMap(snap, self.settings, ignore=(target[0], target[1], TARGET_R_M))
            reach = lim.stop_range_m + lim.cam_x_m  # the chase stops this far from the ball's centre anyway
            route = self.routes.route(clear, (at[0], at[1]), target, goal_ok=True, near_m=reach)
            want = (
                STOP
                if route is None
                else planner.choose(at, self.cmd, aim_point(route, (at[0], at[1])), obstacle, fan, d_lens)
            )
        give_up = BLOCKED_GIVE_UP_S if fan is None else self.settings.stuck_s
        if want == STOP:  # every curve blocked (design-notes §157.5), or no route
            if self._blocked_t0 is None:
                self._blocked_t0 = now_s
        else:
            self._blocked_t0 = None
        blocked = self._blocked_t0 is not None and now_s - self._blocked_t0 >= give_up - 1e-9
        if fan is None and blocked:
            return self._end("blocked")  # slice 1, no map
        if fan is not None and (blocked or not self._made_progress(now_s, at)):  # stuck on the map (§18.13.4)
            self._blocked_t0, self._progress, self._recovering = None, None, True
            self.recovery.start(now_s, at)
            self.cmd = STOP
            return SkillOut(STOP)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        if (
            want == STOP
        ):  # blocked: stop at once, not by the speed ramp (Plan Z5: the ramp coasted into things)
            self.cmd = STOP
            return SkillOut(STOP, cam)
        return self._out(want, cam)

    def _made_progress(self, now_s: float, at: XYTh) -> bool:
        """False once the tank has neither moved MOVED_M nor turned TURNED_DEG for stuck_s (on the map)."""
        if self._progress is None:
            self._progress = (at, now_s)
            return True
        p, t = self._progress
        turned = abs((math.degrees(at[2] - p[2]) + 180.0) % 360.0 - 180.0)
        if math.dist(p[:2], at[:2]) >= MOVED_M or turned >= TURNED_DEG:
            self._progress = (at, now_s)
            self.recovery.progress(at)
            return True
        return now_s - t < self.settings.stuck_s - 1e-9

    def _recover(self, now_s: float, at: XYTh, cost: CostMap) -> SkillOut:
        out = self.recovery.tick(now_s, at, cost, self._dt)
        if self.recovery.step == "blocked":
            return self._end("blocked")
        if out is None:  # a step finished: approach again
            self._recovering, self._pan = False, None
            self.cmd = STOP
            return SkillOut(STOP)
        cmd, cam = out
        if cmd == STOP:
            self.cmd = STOP
            return SkillOut(STOP, cam)
        return self._out(cmd, cam)
