"""Roam the room on the map (AI engine LLD §18.7, §18.13, spec 9 §4): wander, explore (visit everything) and
find.

Each tick: one cost map from the snapshot, the goal's destination, a route on the costs, and slice 1's fan
aiming 0.5 m along it, scored and slowed by cost (Plan Z6). Stuck - no allowed move or no progress for
stuck_s, or no route - the standard ways out run (`Recovery`: look, turn, back over known floor, blocked).
Find hands over to the chase once the thing is seen; it searches ("search": the nearest floor the camera
hasn't looked at from near) and ends not_found when every reachable spot has been (design-notes §188).

Safety (spec 9 §5): a stale map stops the tank at once (2 s ends the run); an echo nearer than clear_m that
isn't the target ends the run; 18 cm/s while the camera angle isn't trusted or there is no depth."""

from __future__ import annotations

import math
import random
from collections.abc import Callable, Sequence
from dataclasses import dataclass, replace
from typing import Literal

from conqueror_ai.domain.drive.candidates import CLOSE_M, Candidate, Candidates
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
from conqueror_ai.domain.drive.goals import ExploreGoal, SearchGoal, WanderGoal
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.recovery import ProgressWatch, Recovery
from conqueror_ai.domain.drive.route import XY, RoutePlanner, aim_point
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.room_map import MapSnapshot

SLOW_CMS = 18.0  # spec 9 §5: unsure of the camera angle, or no depth
# find: look again at a candidate (owner, design-notes §179, §189)
CHECK_LOOK_S = 1.5  # standing, the camera on the spot, still pictures this long
CHECK_CLOSE_M = CLOSE_M  # not confirmed from further: drive this near and look once more
CHECK_MAX_S = 15.0  # a whole check never longer
CANDIDATE_RETRY_S = 1.0  # pending candidates with no way to them: tried again after this long
PLAN_TRIES = 3  # destinations tried in one tick before it counts as stuck
START_STAND_S = 1.0  # at the start: stand with the camera ahead so the map takes still pictures


Mode = Literal["wander", "explore", "search"]


@dataclass
class _Check:
    """One candidate being checked: look still, go closer, look again (§179, §189)."""

    cand: Candidate
    t0: float
    phase: Literal["look", "closer"] = "look"
    closer: bool = False  # it has been driven closer already
    aimed: bool = False  # the camera was sent to the spot
    still_t0: float | None = None  # since when the pictures are still

    @property
    def spot(self) -> XY:
        return self.cand.spot  # the newest sighting's spot


class RoamSkill:
    def __init__(
        self,
        planner: Planner,
        mode: Mode,
        seconds: float | None = None,
        find: bool = False,
        rng: random.Random | None = None,
        cost: CostSettings | None = None,
        area_m: float | None = None,
        memory: ThingMemory | None = None,
        kind: Callable[[str], bool] | None = None,
    ) -> None:
        """area_m: stay within this many metres of where the run starts (None: anywhere it can reach). Find:
        memory = the things confirmed so far (its spots of `kind` are checked first; one not there is marked
        gone), kind = the thing sought (§189)."""
        self.planner = planner
        self.slow = Planner(replace(planner.lim, v_cap_cms=min(planner.lim.v_cap_cms, SLOW_CMS)), planner.w)
        self.lim = planner.lim
        self.settings = cost or CostSettings()
        self.mode = mode
        self.find = find
        self.routes = RoutePlanner(self.lim)
        self.wander = WanderGoal(rng or random.Random(), self.lim)
        self.seconds = seconds
        self.explore = ExploreGoal()
        self.search = SearchGoal()
        self._look_pan: float | None = None  # where the camera was last sent (find's search marks its view)
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
        self._progress = ProgressWatch(self.recovery, self.settings.stuck_s)
        self._cost: tuple[MapSnapshot, CostMap] | None = None
        self.area_m = area_m
        self._area: tuple[float, float, float] | None = None  # (x, y, r) once the start is known
        self._check: _Check | None = None
        self.candidates = Candidates()
        self.memory = memory
        self.kind = kind
        self._seeded = False
        self._retry_s = -math.inf  # no pending candidate reachable: not tried again before this

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
            self._cost = (snap, CostMap(snap, self.settings, area=self._area))
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
        maybes: Sequence[XY] = (),
    ) -> SkillOut:
        """One tick: at = the pose (x, y, th), snap = the room map, fix = the thing to find (find only),
        sonar_m = the echo from the tank's front (None: none), map_ok = pictures and telemetry fresh, maybes =
        the newest picture's sightings of the thing on the floor, sure or not (find only: candidates,
        §189). In a search a still picture marks the camera's near view as searched (§188)."""
        if self.mode == "search" and map_ok and at is not None and cam_still and self._look_pan is not None:
            self.search.look(at, self._look_pan, self.lim.cam_x_m)
        if self.find:
            for spot in maybes:
                self.candidates.add(spot)
            if not self._seeded:  # remembered spots first: no look round when it knows where to look
                self._seeded = True
                if self.memory is not None and self.kind is not None:
                    self.candidates.seed(self.memory.of_kind(self.kind))
                    if self.candidates.pending():
                        self.state = "drive"
        out = self._tick(now_s, at, snap, fix, sonar_m, map_ok, cam_still)
        if out.cam is not None:
            self._look_pan = out.cam[0]
        return out

    def _tick(
        self,
        now_s: float,
        at: XYTh | None,
        snap: MapSnapshot | None,
        fix: TargetFix | None,
        sonar_m: float | None,
        map_ok: bool,
        cam_still: bool = True,
    ) -> SkillOut:
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
        if self.area_m is not None and self._area is None:  # the play area: round where the run starts
            self._area = (at[0], at[1], self.area_m)
            self.chase.area = self._area
        # find (§187): first the chase's look round from where it stands, then search; seen: go to it
        seen = fix is not None and fix.seen_age_s < LOST_AFTER_S
        if self.find and (self.state in ("start", "chase") or seen):
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
        planner = self.planner if snap.angle_trusted and snap.depth_ok else self.slow
        if self.find and self.state == "drive" and self.candidates.pending() and now_s >= self._retry_s:
            c = self.candidates.next(at, snap, self.routes, self._area, self.settings)
            if c is None:
                self._retry_s = now_s + CANDIDATE_RETRY_S
            else:
                self._check, self.state = _Check(c, now_s), "check"
                self._progress.reset()
        if self.state == "check":
            return self._checking(now_s, at, snap, planner, sonar_m, cam_still)
        if self.state == "recover":
            return self._recover(now_s, at, cost)
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
            self.chase.area = self._area
            self.state = "drive"
            self._progress.reset()
            return self._halt((0.0, DRIVE_TILT))
        if out.result is not None:
            return self._end(out.result)
        return out

    # ---- where to
    def _destination(self, now_s: float, at: XYTh, cost: CostMap) -> XY | None:
        if self.mode == "wander":
            return self.wander.destination(now_s, at, cost, self.routes)
        if self.mode == "search":
            return self.search.destination(now_s, at, cost, self.routes)
        return self.explore.destination(now_s, at, cost, self.routes)

    def _plan(self, now_s: float, at: XYTh, cost: CostMap) -> list[XY] | None:
        """A destination and a route to it, or None."""
        for _ in range(PLAN_TRIES):  # an unreachable spot (the map changed since it was picked) is dropped
            dest = self._destination(now_s, at, cost)
            if dest is None:
                return None
            route = self.routes.route(cost, (at[0], at[1]), dest)
            if route is not None:
                self.dest = dest
                return route
            if self.mode == "wander":
                self.wander.drop()
            elif self.mode == "search":
                self.search.drop()
            else:
                self.explore.drop()
        return None

    # ---- stuck: the standard ways out
    def _stuck(self, now_s: float, at: XYTh) -> SkillOut:
        self.recovery.start(now_s, at)
        self.state = "recover"
        self._progress.reset()
        if self.mode == "wander":
            self.wander.drop()
        return self._halt()

    def _recover(self, now_s: float, at: XYTh, cost: CostMap) -> SkillOut:
        out = self.recovery.tick(now_s, at, cost)
        if self.recovery.step == "blocked":
            return self._end("blocked")
        if out is None:  # a step finished: try driving again
            self.state, self._pan = "drive", 0.0
            return self._halt((0.0, DRIVE_TILT))
        cmd, cam = out
        if cam is not None:
            self._pan = cam[0]
        return self._halt(cam) if cmd == STOP else self._out(cmd, cam)

    # ---- look again at a candidate (find, §179, §189)
    def _end_check(self, now_s: float) -> SkillOut:
        """Not confirmed: ruled out for the run (a remembered thing: gone), search on."""
        if self._check is not None:
            c = self._check.cand
            self.candidates.rule_out(c)
            if c.remembered is not None and self.memory is not None:
                self.memory.mark_gone(c.remembered)
        self._check, self.state, self._pan = None, "drive", 0.0
        self._progress.reset()
        return self._halt((0.0, DRIVE_TILT))

    def _checking(
        self,
        now_s: float,
        at: XYTh,
        snap: MapSnapshot,
        planner: Planner,
        sonar_m: float | None,
        cam_still: bool,
    ) -> SkillOut:
        """Stand with the camera on the spot until the pictures have been still CHECK_LOOK_S; not confirmed
        (the chase takes over when it is) and further than CHECK_CLOSE_M: drive that near, look once more."""
        c = self._check
        if c is None or now_s - c.t0 >= CHECK_MAX_S - 1e-9:
            return self._end_check(now_s)
        d = math.dist(at[:2], c.spot)
        b = bearing_deg(at, c.spot)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        if c.phase == "look":
            cam = None
            if not c.aimed:
                c.aimed, c.still_t0, self._pan = True, None, pan
                cam = (pan, DRIVE_TILT)
            elif not cam_still:
                c.still_t0 = None
            elif c.still_t0 is None:
                c.still_t0 = now_s
            if c.still_t0 is not None and now_s - c.still_t0 >= CHECK_LOOK_S - 1e-9:
                if c.closer or d <= CHECK_CLOSE_M + 0.05:
                    return self._end_check(now_s)
                c.phase = "closer"
            return self._halt(cam)
        if d <= CHECK_CLOSE_M:
            c.phase, c.closer, c.aimed = "look", True, False
            return self._halt()
        # the way: its own marks set aside, inside the play area (§189); the fan still keeps off every mark
        route = self.candidates.route(c.cand, at, snap, self.routes, self._area, self.settings)
        cost = self._costmap(snap)
        if route is None or not self._progress.ok(now_s, at):
            return self._end_check(now_s)
        obstacle = None if sonar_m is None else sonar_m + self.lim.sonar_x_m
        want = planner.choose(at, self.cmd, aim_point(route, (at[0], at[1])), obstacle, cost, far_m=math.inf)
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._halt(cam) if want == STOP else self._out(want, cam)

    # ---- driving
    def _drive(
        self, now_s: float, at: XYTh, cost: CostMap, planner: Planner, sonar_m: float | None
    ) -> SkillOut:
        route = self._plan(now_s, at, cost)
        if route is None:
            if self.mode == "explore" and self.explore.done:
                return self._end("done")
            if self.mode == "search" and self.search.done:  # every spot looked at, never seen (§188)
                return self._end("not_found")
            return self._stuck(now_s, at)
        self.route = route
        aim = aim_point(route, (at[0], at[1]))
        obstacle = None if sonar_m is None else sonar_m + self.lim.sonar_x_m
        # a wander or explore destination is a waypoint, not a stop: cruise (the costs slow it near things)
        want = planner.choose(at, self.cmd, aim, obstacle, cost, far_m=math.inf)
        if not self._progress.ok(now_s, at):
            return self._stuck(now_s, at)
        b = bearing_deg(at, aim)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, DRIVE_TILT)
        return self._halt(cam) if want == STOP else self._out(want, cam)
