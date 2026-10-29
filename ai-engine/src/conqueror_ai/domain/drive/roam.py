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
LOOK_PANS = (-45.0, 45.0, -90.0, 90.0)  # too near things to turn on the spot: look round with the camera
STUCK_M = 0.2  # a look that starts within this of the last one made no progress ...
STUCK_LOOKS = 3  # ... this many in a row end the run (review minor 8: drive-blocked-look cycles)

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
        self._pan_look = False  # this look step pans the camera instead of turning the tank
        self._last_look_at: XY | None = None
        self._stuck = 0
        # per snapshot: routes and goals see open unseen space as open; the fan also counts unseen space next
        # to seen marks as the thing (review I3: growing it for routes closed doorways)
        self._clear: tuple[MapSnapshot, ClearanceMap, ClearanceMap] | None = None

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

    def _clearances(self, snap: MapSnapshot) -> tuple[ClearanceMap, ClearanceMap]:
        """(routes and goals, the fan) - once per snapshot (kept by identity, review minor 5)."""
        if self._clear is None or self._clear[0] is not snap:
            self._clear = (snap, ClearanceMap(snap, grow=False), ClearanceMap(snap))
        return self._clear[1], self._clear[2]

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
        clear, fan = self._clearances(snap)
        if self.state == "start":
            self.state = "look"
            return self._start_look(now_s, at)
        if self.state == "look":
            return self._look(now_s, at, clear, fan, planner, sonar_m)
        return self._drive(now_s, at, clear, fan, planner, sonar_m)

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
            self.chase = ChaseSkill(self.planner)
            self.state = "look"
            self.cmd = STOP
            return self._start_look(now_s, at)
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
    def _start_look(self, now_s: float, at: XYTh) -> SkillOut:
        """Stand with the camera ahead, then look round (LLD §18.7). Stops at once; counts looks that start
        where the last one did (no progress)."""
        self._look_t0, self._turned, self._spinning, self._pan_look = now_s, 0.0, False, False
        if self._last_look_at is not None and math.dist(self._last_look_at, at[:2]) < STUCK_M:
            self._stuck += 1
        else:
            self._stuck = 0
        self._last_look_at = (at[0], at[1])
        self._pan = 0.0
        self.cmd = STOP
        return SkillOut(STOP, (0.0, DRIVE_TILT))

    def _look(
        self,
        now_s: float,
        at: XYTh,
        clear: ClearanceMap,
        fan: ClearanceMap,
        planner: Planner,
        sonar_m: float | None,
    ) -> SkillOut:
        if self._stuck >= STUCK_LOOKS:
            return self._end("blocked")
        if self._spinning:
            self._turned += abs(self.cmd.w_dps) * self._dt  # what the tank was told last tick
            if self._turned >= self._step_to - 1e-9 or not planner.can_spin(at, fan):
                self._spinning, self._look_t0 = False, now_s  # one step done (or too near): stand and look
                return self._halt()
            return self._out(Command(0.0, self.lim.spin_dps))
        if now_s - self._look_t0 < LOOK_STAND_S - 1e-9:
            return self._halt()
        route = self._plan(now_s, at, clear)
        if route is not None:
            self.route, self.state, self._looks, self._blocked_t0 = route, "drive", 0, None
            out = self._drive(now_s, at, clear, fan, planner, sonar_m)
            return (
                out if out.cam is not None else SkillOut(out.cmd, (self._pan or 0.0, DRIVE_TILT), out.result)
            )
        if self.mode == "explore" and self.explore.done and self._turned >= 360.0 - 1e-9:
            return self._end("done")
        if self._turned >= 360.0 - 1e-9:
            self._looks += 1
            if self._looks >= LOOK_ROUNDS:
                return self._end("done" if self.mode == "explore" else "blocked")
            self._turned = 0.0
            self.wander.drop()
        self._step_to = self._turned + LOOK_STEP_DEG
        if planner.can_spin(at, fan):
            self._spinning = True
            return self._out(Command(0.0, self.lim.spin_dps))
        # too near something to turn on the spot (review C1): a quarter of the look with the camera instead
        k = round(self._turned / LOOK_STEP_DEG) % len(LOOK_PANS)
        self._turned, self._look_t0, self._pan = self._step_to, now_s, LOOK_PANS[k]
        return self._halt((LOOK_PANS[k], DRIVE_TILT))

    # ---- driving
    def _drive(
        self,
        now_s: float,
        at: XYTh,
        clear: ClearanceMap,
        fan: ClearanceMap,
        planner: Planner,
        sonar_m: float | None,
    ) -> SkillOut:
        route = self._plan(now_s, at, clear)
        if route is None:
            self.state, self.route = "look", None
            return self._start_look(now_s, at)
        self.route = route
        aim = aim_point(route, (at[0], at[1]))
        obstacle = None if sonar_m is None else sonar_m + self.lim.sonar_x_m
        # a wander or explore destination is a waypoint, not a stop: cruise (the map and sonar keep it clear)
        want = planner.choose(at, self.cmd, aim, obstacle, fan, far_m=math.inf)
        if want == STOP:
            if self._blocked_t0 is None:
                self._blocked_t0 = now_s
            if now_s - self._blocked_t0 >= BLOCKED_GIVE_UP_S - 1e-9:  # stuck: look round for another way
                self.state, self._blocked_t0 = "look", None
                if self.mode == "wander":
                    self.wander.drop()
                return self._start_look(now_s, at)
        else:
            self._blocked_t0 = None
        b = bearing_deg(at, aim)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._halt(cam) if want == STOP else self._out(want, cam)
