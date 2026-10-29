"""Roam the room on the map (AI engine LLD §18.7, §18.13, spec 9 §4): wander, explore (visit everything) and
find.

Each tick: one cost map from the snapshot, the goal's destination, a route on the costs, and slice 1's fan
aiming 0.5 m along it, scored and slowed by cost (Plan Z6). Stuck - no allowed move or no progress for
stuck_s, or no route - the standard ways out run (`Recovery`: look, turn, back over known floor, blocked).
Find hands over to the chase once the thing is seen.

Safety (spec 9 §5): a stale map stops the tank at once (2 s ends the run); an echo nearer than clear_m that
isn't the target ends the run; 18 cm/s while the camera angle isn't trusted or there is no depth."""

from __future__ import annotations

import math
import random
from dataclasses import replace
from typing import Literal

from conqueror_ai.domain.drive.chase import (
    DRIVE_TILT,
    LOST_AFTER_S,
    MAX_TICK_S,
    PAN_LIMIT_DEG,
    PAN_STEP_DEG,
    STALE_GIVE_UP_S,
    ChaseSkill,
    SkillOut,
)
from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.goals import ExploreGoal, WanderGoal
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.recovery import Recovery
from conqueror_ai.domain.drive.route import XY, RoutePlanner, aim_point
from conqueror_ai.domain.room_map import MapSnapshot

SLOW_CMS = 18.0  # spec 9 §5: unsure of the camera angle, or no depth
START_STAND_S = 1.0  # at the start: stand with the camera ahead so the map takes still pictures
PROGRESS_M = 0.05  # moving this far ...
PROGRESS_DEG = 15.0  # ... or turning this far counts as progress (no progress for stuck_s: stuck)

Mode = Literal["wander", "explore"]


class RoamSkill:
    def __init__(
        self,
        planner: Planner,
        mode: Mode,
        seconds: float | None = None,
        find: bool = False,
        rng: random.Random | None = None,
        cost: CostSettings | None = None,
    ) -> None:
        self.planner = planner
        self.slow = Planner(replace(planner.lim, v_cap_cms=min(planner.lim.v_cap_cms, SLOW_CMS)), planner.w)
        self.lim = planner.lim
        self.settings = cost or CostSettings()
        self.mode = mode
        self.find = find
        self.routes = RoutePlanner(self.lim)
        self.wander = WanderGoal(seconds, rng or random.Random())
        self.seconds = seconds
        self.explore = ExploreGoal()
        self.chase = ChaseSkill(planner, cost)
        self.recovery = Recovery(self.lim, planner.foot, DRIVE_TILT)
        self.state = "start"
        self.cmd = STOP
        self.route: list[XY] | None = None
        self.dest: XY | None = None
        self._t0: float | None = None
        self._last_s: float | None = None
        self._dt = planner.lim.dt_s
        self._pan: float | None = None
        self._stale_t0: float | None = None
        self._start_t0: float | None = None
        self._progress: tuple[XYTh, float] | None = None  # where and when it last made progress
        self._cost: tuple[MapSnapshot, CostMap] | None = None

    # ---- out
    def _out(self, want: Command, cam: tuple[float, float] | None = None) -> SkillOut:
        self.cmd = ramp(self.cmd, want, self._dt, self.lim)
        return SkillOut(self.cmd, cam)

    def _halt(self, cam: tuple[float, float] | None = None) -> SkillOut:
        """Stop at once, not by the speed ramp: the way ahead is blocked (simulated wander: the ramp coasted
        the tank 2 s on into a bed edge)."""
        self.cmd = STOP
        return SkillOut(STOP, cam)

    def _end(self, result: str) -> SkillOut:
        self.state = "done" if result == "done" else "failed"
        self.cmd = STOP
        return SkillOut(STOP, None, result)

    def cancel(self) -> SkillOut:
        return self._end("cancelled")

    def _costmap(self, snap: MapSnapshot) -> CostMap:
        """One cost map per snapshot (kept by identity, review minor 5)."""
        if self._cost is None or self._cost[0] is not snap:
            self._cost = (snap, CostMap(snap, self.settings))
        return self._cost[1]

    # ---- the tick
    def tick(
        self,
        now_s: float,
        at: XYTh | None,
        snap: MapSnapshot | None,
        fix: TargetFix | None,
        sonar_m: float | None,
        map_ok: bool,
        cam_still: bool = True,
    ) -> SkillOut:
        """One tick: at = the pose (x, y, th), snap = the room map, fix = the thing to find (find only),
        sonar_m = the echo from the tank's front (None: none), map_ok = pictures and telemetry fresh."""
        if self.state in ("done", "failed"):
            return SkillOut(STOP)
        if self._last_s is not None:
            self._dt = min(max(now_s - self._last_s, 0.0), MAX_TICK_S)
        self._last_s = now_s
        if self._t0 is None:
            self._t0 = now_s
        if not map_ok or at is None or snap is None:  # it can't see: stop at once; 2 s of it ends the run
            if self._stale_t0 is None:
                self._stale_t0 = now_s
            if now_s - self._stale_t0 >= STALE_GIVE_UP_S - 1e-9:
                return self._end("stale")
            self.cmd = STOP
            return SkillOut(STOP)
        self._stale_t0 = None
        if self.find and (self.state == "chase" or (fix is not None and fix.seen_age_s < LOST_AFTER_S)):
            return self._chase(now_s, at, snap, fix, sonar_m, map_ok, cam_still)
        if self.mode == "wander" and self.seconds is not None and now_s - self._t0 >= self.seconds - 1e-9:
            return self._end("done")
        if sonar_m is not None and sonar_m < self.lim.clear_m:
            return self._end("blocked")
        cost = self._costmap(snap)
        if self.state == "start":
            if self._start_t0 is None:
                self._start_t0, self._pan = now_s, 0.0
                return self._halt((0.0, DRIVE_TILT))
            if now_s - self._start_t0 < START_STAND_S - 1e-9:
                return self._halt()
            self.state = "drive"
        if self.state == "recover":
            return self._recover(now_s, at, cost)
        planner = self.planner if snap.angle_trusted and snap.depth_ok else self.slow
        return self._drive(now_s, at, cost, planner, sonar_m)

    def _chase(
        self,
        now_s: float,
        at: XYTh,
        snap: MapSnapshot,
        fix: TargetFix | None,
        sonar_m: float | None,
        map_ok: bool,
        cam_still: bool,
    ) -> SkillOut:
        if self.state != "chase":
            self.state = "chase"
            self.chase.cmd = self.cmd  # it is moving: the chase's plans start from that (review minor 4)
        out = self.chase.tick(now_s, at, fix, sonar_m, map_ok, cam_still, snap)
        self.cmd = out.cmd
        if out.result in ("not_found", "lost"):  # it got away: roam on, a fresh chase for the next sighting
            self.chase = ChaseSkill(self.planner, self.settings)
            self.state, self._progress = "drive", None
            return self._halt((0.0, DRIVE_TILT))
        if out.result is not None:
            return self._end(out.result)
        return out

    # ---- where to
    def _destination(self, now_s: float, at: XYTh, cost: CostMap) -> XY | None:
        if self.mode == "wander":
            return self.wander.destination(now_s, at, cost)
        return self.explore.destination(now_s, at, cost, self.routes)

    def _plan(self, now_s: float, at: XYTh, cost: CostMap) -> list[XY] | None:
        """A destination and a route to it, or None."""
        for _ in range(3):  # wander: an unreachable spot is dropped and another picked
            dest = self._destination(now_s, at, cost)
            if dest is None:
                return None
            route = self.routes.route(cost, (at[0], at[1]), dest)
            if route is not None:
                self.dest = dest
                return route
            if self.mode != "wander":
                return None
            self.wander.drop()
        return None

    # ---- stuck: the standard ways out
    def _stuck(self, now_s: float, at: XYTh) -> SkillOut:
        self.recovery.start(now_s, at)
        self.state, self._progress = "recover", None
        if self.mode == "wander":
            self.wander.drop()
        return self._halt()

    def _recover(self, now_s: float, at: XYTh, cost: CostMap) -> SkillOut:
        out = self.recovery.tick(now_s, at, cost, self._dt)
        if self.recovery.step == "blocked":
            return self._end("blocked")
        if out is None:  # a step finished: try driving again
            self.state, self._pan = "drive", 0.0
            return self._halt((0.0, DRIVE_TILT))
        cmd, cam = out
        if cam is not None:
            self._pan = cam[0]
        return self._halt(cam) if cmd == STOP else self._out(cmd, cam)

    # ---- driving
    def _made_progress(self, now_s: float, at: XYTh) -> bool:
        """False once the tank has neither moved PROGRESS_M nor turned PROGRESS_DEG for stuck_s."""
        if self._progress is None:
            self._progress = (at, now_s)
            return True
        p, t = self._progress
        turned = abs((math.degrees(at[2] - p[2]) + 180.0) % 360.0 - 180.0)
        if math.dist(p[:2], at[:2]) >= PROGRESS_M or turned >= PROGRESS_DEG:
            self._progress = (at, now_s)
            self.recovery.progress(at)
            return True
        return now_s - t < self.settings.stuck_s - 1e-9

    def _drive(
        self, now_s: float, at: XYTh, cost: CostMap, planner: Planner, sonar_m: float | None
    ) -> SkillOut:
        route = self._plan(now_s, at, cost)
        if route is None:
            if self.mode == "explore" and self.explore.done:
                return self._end("done")
            return self._stuck(now_s, at)
        self.route = route
        aim = aim_point(route, (at[0], at[1]))
        obstacle = None if sonar_m is None else sonar_m + self.lim.sonar_x_m
        # a wander or explore destination is a waypoint, not a stop: cruise (the costs slow it near things)
        want = planner.choose(at, self.cmd, aim, obstacle, cost, far_m=math.inf)
        if not self._made_progress(now_s, at):
            return self._stuck(now_s, at)
        b = bearing_deg(at, aim)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._halt(cam) if want == STOP else self._out(want, cam)
