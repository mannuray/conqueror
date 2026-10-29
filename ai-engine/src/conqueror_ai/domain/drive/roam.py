"""Roam the room on the map (AI engine LLD §18.7, spec 9 §4): wander, explore (visit everything) and find.

Each tick: the snapshot's clearance, the goal's destination, a route around obstacles, and slice 1's fan
aiming 0.5 m along it. No destination or no route: a look (stand, then turn on the spot 90 deg at a time and
stand again, so the map takes still pictures). Find hands over to the chase once the thing is seen.

Safety (spec 9 §5): a stale map stops the tank at once (2 s ends the run); an echo nearer than clear_m that
isn't the target ends the run; 18 cm/s while the camera angle isn't trusted or there is no depth."""

from __future__ import annotations

import math
import random
from dataclasses import replace
from typing import Literal

from conqueror_ai.domain.drive.chase import (
    BLOCKED_GIVE_UP_S,
    DRIVE_TILT,
    LOST_AFTER_S,
    MAX_TICK_S,
    PAN_LIMIT_DEG,
    PAN_STEP_DEG,
    STALE_GIVE_UP_S,
    ChaseSkill,
    SkillOut,
)
from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.goals import ExploreGoal, WanderGoal
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.route import XY, RoutePlanner, aim_point
from conqueror_ai.domain.room_map import MapSnapshot

SLOW_CMS = 18.0  # spec 9 §5: unsure of the camera angle, or no depth
LOOK_STAND_S = 1.0  # stand this long so the map takes still pictures
LOOK_STEP_DEG = 90.0
LOOK_ROUNDS = 2  # full looks round without anywhere to go: wander ends blocked, explore done

Mode = Literal["wander", "explore"]


class RoamSkill:
    def __init__(
        self,
        planner: Planner,
        mode: Mode,
        seconds: float | None = None,
        find: bool = False,
        rng: random.Random | None = None,
    ) -> None:
        self.planner = planner
        self.slow = Planner(replace(planner.lim, v_cap_cms=min(planner.lim.v_cap_cms, SLOW_CMS)), planner.w)
        self.lim = planner.lim
        self.mode = mode
        self.find = find
        self.routes = RoutePlanner()
        self.wander = WanderGoal(seconds, rng or random.Random())
        self.seconds = seconds
        self.explore = ExploreGoal()
        self.chase = ChaseSkill(planner)
        self.state = "start"
        self.cmd = STOP
        self.route: list[XY] | None = None
        self.dest: XY | None = None
        self._t0: float | None = None
        self._last_s: float | None = None
        self._dt = planner.lim.dt_s
        self._pan: float | None = None
        self._stale_t0: float | None = None
        self._blocked_t0: float | None = None
        self._look_t0 = 0.0
        self._turned = 0.0  # degrees turned in this look
        self._looks = 0  # full looks with nowhere to go
        self._spinning = False
        self._step_to = 0.0  # this spin ends when _turned reaches it
        self._clear: tuple[int, ClearanceMap] | None = None

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

    def _clearance(self, snap: MapSnapshot) -> ClearanceMap:
        key = id(snap)
        if self._clear is None or self._clear[0] != key:
            self._clear = (key, ClearanceMap(snap))
        return self._clear[1]

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
        planner = self.planner if snap.angle_trusted and snap.depth_ok else self.slow
        clear = self._clearance(snap)
        if self.state == "start":
            self.state = "look"
            self._start_look(now_s)
            return self._out(STOP, (0.0, DRIVE_TILT))
        if self.state == "look":
            return self._look(now_s, at, clear, planner, sonar_m)
        return self._drive(now_s, at, clear, planner, sonar_m)

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
        self.state = "chase"
        out = self.chase.tick(now_s, at, fix, sonar_m, map_ok, cam_still, snap)
        self.cmd = out.cmd
        if out.result in ("not_found", "lost"):  # it got away: roam on, a fresh chase for the next sighting
            self.chase = ChaseSkill(self.planner)
            self.state = "look"
            self._start_look(now_s)
            return SkillOut(STOP, (0.0, DRIVE_TILT))
        if out.result is not None:
            return self._end(out.result)
        return out

    # ---- where to
    def _destination(self, now_s: float, at: XYTh, clear: ClearanceMap) -> XY | None:
        if self.mode == "wander":
            return self.wander.destination(now_s, at, clear)
        return self.explore.destination(now_s, at, clear, self.routes)

    def _plan(self, now_s: float, at: XYTh, clear: ClearanceMap) -> list[XY] | None:
        """A destination and a route to it, or None."""
        for _ in range(3):  # wander: an unreachable spot is dropped and another picked
            dest = self._destination(now_s, at, clear)
            if dest is None:
                return None
            route = self.routes.route(clear, (at[0], at[1]), dest)
            if route is not None:
                self.dest = dest
                return route
            if self.mode != "wander":
                return None
            self.wander.drop()
        return None

    # ---- looking round
    def _start_look(self, now_s: float) -> None:
        self._look_t0, self._turned, self._spinning = now_s, 0.0, False

    def _look(
        self, now_s: float, at: XYTh, clear: ClearanceMap, planner: Planner, sonar_m: float | None
    ) -> SkillOut:
        if self._spinning:
            self._turned += abs(self.cmd.w_dps) * self._dt  # what the tank was told last tick
            if self._turned >= self._step_to - 1e-9:
                self._spinning, self._look_t0 = False, now_s  # one step done: stand and look
                return self._out(STOP)
            return self._out(Command(0.0, self.lim.spin_dps))
        if now_s - self._look_t0 < LOOK_STAND_S - 1e-9:
            return self._out(STOP)
        route = self._plan(now_s, at, clear)
        if route is not None:
            self.route, self.state, self._looks, self._blocked_t0 = route, "drive", 0, None
            return self._drive(now_s, at, clear, planner, sonar_m)
        if self.mode == "explore" and self.explore.done and self._turned >= 360.0 - 1e-9:
            return self._end("done")
        if self._turned >= 360.0 - 1e-9:
            self._looks += 1
            if self._looks >= LOOK_ROUNDS:
                return self._end("done" if self.mode == "explore" else "blocked")
            self._turned = 0.0
            self.wander.drop()
        self._spinning, self._step_to = True, self._turned + LOOK_STEP_DEG
        return self._out(Command(0.0, self.lim.spin_dps))

    # ---- driving
    def _drive(
        self, now_s: float, at: XYTh, clear: ClearanceMap, planner: Planner, sonar_m: float | None
    ) -> SkillOut:
        route = self._plan(now_s, at, clear)
        if route is None:
            self.state, self.route = "look", None
            self._start_look(now_s)
            return self._halt()
        self.route = route
        aim = aim_point(route, (at[0], at[1]))
        obstacle = None if sonar_m is None else sonar_m + self.lim.sonar_x_m
        # a wander or explore destination is a waypoint, not a stop: cruise (the map and sonar keep it clear)
        want = planner.choose(at, self.cmd, aim, obstacle, clear, far_m=math.inf)
        if want == STOP:
            if self._blocked_t0 is None:
                self._blocked_t0 = now_s
            if now_s - self._blocked_t0 >= BLOCKED_GIVE_UP_S - 1e-9:  # stuck: look round for another way
                self.state, self._blocked_t0 = "look", None
                if self.mode == "wander":
                    self.wander.drop()
                self._start_look(now_s)
                return self._halt()
        else:
            self._blocked_t0 = None
        b = bearing_deg(at, aim)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._halt(cam) if want == STOP else self._out(want, cam)
