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

from conqueror_ai.domain.drive.candidates import Candidate, Candidates
from conqueror_ai.domain.drive.chase import (
    LOST_AFTER_S,
    MAX_TICK_S,
    PAN_LIMIT_DEG,
    PAN_STEP_DEG,
    STALE_GIVE_UP_S,
    ChaseSkill,
)
from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.goals import DestinationGoal, ExploreGoal, PointGoal, SearchGoal, WanderGoal
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.recovery import ProgressWatch, Recovery
from conqueror_ai.domain.drive.route import XY, RoutePlanner, aim_point
from conqueror_ai.domain.drive.skill import Result, SkillInput, SkillOut
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.objects import Glimpse
from conqueror_ai.domain.reach import CHECK_CLOSE_M, SAME_SPOT_M
from conqueror_ai.domain.room_map import MapSnapshot

# find: look again at a candidate (owner, design-notes §179, §189)
CHECK_LOOK_S = 1.5  # standing, the camera on the spot, still pictures this long
CHECK_MAX_S = 15.0  # a check never longer than this ...
CHECK_S_PER_M = 10.0  # ... plus this per metre it has to drive closer (review I3: 15 s covered the drive too)
CHECK_NEAR_TOL_M = 0.05  # within CHECK_CLOSE_M and this counts as near (the way ends at about 0.5 m)
CHECK_HITS = 2  # pictures in a close look with a maybe at the spot that confirm it (LLD §24 B, §215)
CHECK_TRIES = 3  # checks that could not get near: then ruled out (a remembered thing is not marked gone)
EDGE_IN_M = 0.3  # a thing seen outside the play area: its edge candidate this far inside the edge (§196)
CANDIDATE_RETRY_S = 1.0  # pending candidates with no way to them: tried again after this long
PLAN_TRIES = 3  # destinations tried in one tick before it counts as stuck
START_STAND_S = 1.0  # at the start: stand with the camera ahead so the map takes still pictures
POINT_DONE_M = 0.15  # setxy: the tank's centre this near the point is there (play tolerance, spec 0 §1.1)
NO_ROUTE_S = 5.0  # setxy: no route to the point for this long ends no_way
POINT_PROGRESS_M = 0.3  # setxy: a nearer best distance by this much is progress ...
POINT_PATIENCE_S = 40.0  # ... and this long without any ends no_way (unseen floor is passable: a point behind
# an unseen wall is explored toward until here)
POINT_SLOW_M = 0.5  # setxy: the last stretch at the slow speed, so it stops where it was sent
# find: the second look at a glimpse, a sighting while the camera moved (LLD §25 A2, design-notes §217)
SECOND_LOOK_S = 1.5  # still pictures this long at each look
SECOND_LOOK_MAX_S = 4.0  # a look never longer than this (a camera that never settles, as the chase's sweep)
ONE_LOOK_DEG = 50.0  # a range this wide is one look at its middle (the lens sees ~82 deg); wider: each end
SECOND_LOOK_PAN_DEG = 90.0  # the camera turns this far; beyond, the tank turns on the spot first
GLIMPSE_SAME_DEG = 20.0  # a glimpse this near a bearing already looked at ...
GLIMPSE_FORGET_S = 15.0  # ... from within SAME_SPOT_M, in the last this long, is not looked at again
SECOND_LOOK_GAP_S = 3.0  # at most one second look this often
SECOND_TURN_S = 3.0  # a turn toward a look that hasn't got there by now: the camera as far as it goes (a
# slipping track; the ways out's TURN_S)
GLIMPSE_KNOWN_M = (
    4.0  # a glimpse toward the spot being checked, or a candidate this near, is that one (review I1)
)
NEARER_FACTOR = 0.6  # driving to a check, a pending candidate nearer than this times its distance ...
NEARER_BY_M = 0.5  # ... and by at least this much is checked first (LLD §25 B)


Mode = Literal["wander", "explore", "find", "point"]  # find: the look round, candidates, search, chase


@dataclass
class _Check:
    """One candidate being checked: look still, go closer, look again (§179, §189)."""

    cand: Candidate
    t0: float
    cap_s: float = CHECK_MAX_S  # the check's time
    phase: Literal["look", "closer"] = "look"
    closer: bool = False  # it has been driven closer already
    aimed: bool = False  # the camera was sent to the spot
    still_t0: float | None = None  # since when the pictures are still
    hits: int = 0  # pictures in the look with a maybe at the spot (§24 B)
    last_maybe: XY | None = None  # the newest of them

    @property
    def spot(self) -> XY:
        return self.cand.spot  # the newest sighting's spot


@dataclass
class _SecondLook:
    """A second look where a glimpse may be: each bearing in turn, the camera on it, still pictures."""

    bearings: list[float]  # map bearings, radians
    back: str  # the state to go back to
    t0: float  # when it began
    i: int = 0
    turn_t0: float | None = None  # this look's turn toward it began
    look_t0: float | None = None  # this look's start (the camera sent)
    still_t0: float | None = None


class RoamSkill:
    def __init__(
        self,
        planner: Planner,
        mode: Mode,
        seconds: float | None = None,
        rng: random.Random | None = None,
        cost: CostSettings | None = None,
        area_m: float | None = None,
        memory: ThingMemory | None = None,
        kind: Callable[[str], bool] | None = None,
        point: tuple[float, float] | None = None,
        centre: tuple[float, float] | None = None,
    ) -> None:
        """area_m: stay within this many metres of where the run starts (None: anywhere it can reach). Find:
        memory = the things confirmed so far (its spots of `kind` are checked first; one not there is marked
        gone), kind = the thing sought (§189). point: setxy's map point (mode "point": a route to it on the
        cost map, done within POINT_DONE_M, no route for NO_ROUTE_S ends no_way). centre: the play area's
        centre (None: round the run's first pose); only with area_m (§21.2)."""
        if mode == "point" and point is None:
            raise ValueError("mode 'point' needs a point")
        self.planner = planner
        self.slow = Planner(
            replace(planner.lim, v_cap_cms=min(planner.lim.v_cap_cms, planner.lim.slow_cms)), planner.w
        )
        self.lim = planner.lim
        self.settings = cost or CostSettings()
        self.mode = mode
        self.find = mode == "find"
        self.routes = RoutePlanner(self.lim)
        self.wander = WanderGoal(rng or random.Random(), self.lim)
        self.seconds = seconds
        self.explore = ExploreGoal()
        self.search = SearchGoal()
        self.point = point
        goals: dict[str, DestinationGoal] = {
            "wander": self.wander,
            "explore": self.explore,
            "find": self.search,
        }
        if point is not None:
            goals["point"] = PointGoal(point)
        self.goal = goals[mode]
        self._look_pan: float | None = None  # where the camera was last sent (find's search marks its view)
        self.chase = ChaseSkill(planner, cost, search_when_lost=False)
        self.recovery = Recovery(self.lim, planner.foot, self.lim.drive_tilt_deg)
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
        self._costs: CostMaps | None = None  # this snapshot's cost maps, the play area in them
        self.area_m = area_m
        self._area: tuple[float, float, float] | None = (  # (x, y, r); round the start once it is known
            None if centre is None or area_m is None else (centre[0], centre[1], area_m)
        )
        self._no_route_t0: float | None = None  # point: since when there is no route
        self._best_m = math.inf  # point: the nearest it has been, as of the last progress ...
        self._best_t = 0.0  # ... and when
        self._check: _Check | None = None
        self.candidates = Candidates()
        self.memory = memory
        self.kind = kind
        self._seeded = False
        self._retry_s = -math.inf  # no pending candidate reachable: not tried again before this
        self._tried_now = False  # the pending candidates were tried this tick
        self._outside_m: float | None = None  # the thing was seen this far from the start, outside the area
        self._maybe_fix: TargetFix | None = None  # from maybes in a close look (§24 B)
        self._maybe_seen_s = 0.0  # ... last seen then
        self._maybes: Sequence[XY] = ()  # this tick's maybes of the thing ...
        self._used = False  # ... and whether its picture is new
        self._glimpses: Sequence[Glimpse] = ()  # this tick's, while the camera moved (§25 A2)
        self._second: _SecondLook | None = None
        self._looked: list[tuple[XY, float, float]] = []  # second looks: where from, the bearing, when
        self._second_s = -math.inf  # the last second look's start

    def _sighting(self, spot: XY) -> None:
        """A candidate there - outside the play area never (review M4)."""
        if self._area is None or math.dist(spot, self._area[:2]) <= self._area[2]:
            self.candidates.add(spot)

    # ---- out
    def _out(self, want: Command, cam: tuple[float, float] | None = None) -> SkillOut:
        self.cmd = ramp(self.cmd, want, self._dt, self.lim)
        return SkillOut(self.cmd, cam)

    def _halt(self, cam: tuple[float, float] | None = None) -> SkillOut:
        """Stop at once, not by the speed ramp: the way ahead is blocked (simulated wander: the ramp coasted
        the tank 2 s on into a bed edge)."""
        self.cmd = STOP
        return SkillOut(STOP, cam)

    def _end(self, result: str | Result, detail: str = "") -> SkillOut:
        self.state = "done" if result == "done" else "failed"
        self.cmd = STOP
        return SkillOut(STOP, None, Result(result), detail)

    def _inside(self, fix: TargetFix | None) -> TargetFix | None:
        """The fix, or None when the thing is outside the play area: then a candidate goes at the area's edge
        nearest it (EDGE_IN_M inside), to look again from there, and not_found will say so (owner, §196)."""
        a = self._area
        if fix is None or a is None:
            return fix
        d = math.dist((fix.x_m, fix.y_m), a[:2])
        if d <= a[2]:
            return fix
        self._outside_m = d
        k = (a[2] - EDGE_IN_M) / d
        self.candidates.add((a[0] + (fix.x_m - a[0]) * k, a[1] + (fix.y_m - a[1]) * k))
        return None

    def debug(self) -> dict[str, object]:
        c = self._check
        return {
            "state": self.state,
            "dest": self.dest,
            "route_len": None if self.route is None else len(self.route),
            "route": self.route,
            "pending": [p.spot for p in self.candidates.pending()],
            "ruled_out": self.candidates.ruled_out(),
            "check": None if c is None else c.spot,
            "check_phase": None if c is None else c.phase,
            "second": None if self._second is None else [math.degrees(b) for b in self._second.bearings],
            "looked": self.search.looked_cells() if self.find else [],
        }

    def cancel(self) -> SkillOut:
        return self._end("cancelled")

    def _costs_for(self, snap: MapSnapshot) -> CostMaps:
        """One set of cost maps per snapshot (kept by identity), the play area in them (LLD §19.3)."""
        if self._costs is None or self._costs.snap is not snap:
            self._costs = CostMaps(snap, self.settings, self._area)
        return self._costs

    def _costmap(self, snap: MapSnapshot) -> CostMap:
        return self._costs_for(snap).base()

    # ---- the tick
    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        """One tick (LLD §19.4). Find only: `inp.fix` the thing, `inp.maybes` the newest picture's
        sightings of it on the floor, sure or not (candidates, §189). `inp.used` = a picture arrived this
        tick and was used: only such a picture marks the camera's near view as searched (§188; I2)."""
        at, snap, fix, sonar_m, map_ok = inp.at, inp.snap, inp.fix, inp.sonar_m, inp.map_ok
        cam_still, maybes, used = inp.cam_still, inp.maybes, inp.used
        if self.find and map_ok and at is not None and used and self._look_pan is not None:
            self.search.look(at, self._look_pan, self.lim.cam_x_m)
        if self.area_m is not None and self._area is None and at is not None:  # play area: round the start
            self._area = (at[0], at[1], self.area_m)
        self._tried_now = False
        if self.find:
            for spot in (
                maybes if inp.fresh else ()
            ):  # a re-read picture is not "seen again" (Z9 final review I2);
                # a picture taken while turning is new too (LLD §25 A1)
                self._sighting(spot)
            if not self._seeded:  # remembered spots first: no look round when it knows where to look
                self._seeded = True
                if self.memory is not None and self.kind is not None:
                    self.candidates.seed(self.memory.of_kind(self.kind))
                    if self.candidates.pending():
                        self.state = "drive"
        c = inp.costs  # the run's cost maps, built once - when they carry this skill's area and settings
        if c is not None and c.area == self._area and c.settings == self.settings:
            self._costs = inp.costs
        self._maybes, self._used, self._glimpses = maybes, used, inp.glimpses
        if fix is not None:  # the tracker's own fix always wins over one made from maybes
            self._maybe_fix = None
        elif (m := self._maybe_fix) is not None:  # each maybe at it moves it there, seen now (§24.5)
            near = [p for p in maybes if math.dist(p, (m.x_m, m.y_m)) <= SAME_SPOT_M]
            if near and used:  # only a new, still picture says it was seen now (Z9 final review M1; a
                # turning picture's maybe is placed roughly - it makes candidates, not a chase target, §25)
                self._maybe_seen_s = now_s
                m = TargetFix(-1, near[-1][0], near[-1][1], 0.0)
            self._maybe_fix = fix = TargetFix(-1, m.x_m, m.y_m, max(0.0, now_s - self._maybe_seen_s))
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
        # find (§187): first the chase's look round from where it stands, then search; seen: go to it
        fix = self._inside(fix)  # seen outside the play area: never chased, its edge checked (§196)
        seen = fix is not None and fix.seen_age_s < LOST_AFTER_S
        if self.find and (self.state in ("start", "chase") or seen):
            self._second = None
            return self._chase(now_s, at, snap, fix, sonar_m, map_ok, cam_still)
        if self.point is not None:
            d = math.dist(at[:2], self.point)
            if d <= POINT_DONE_M:
                return self._end("done")
            if self._best_m - d >= POINT_PROGRESS_M:
                self._best_m, self._best_t = d, now_s
            elif now_s - self._best_t >= POINT_PATIENCE_S - 1e-9:
                return self._end(Result.NO_WAY, "no nearer to the point")
        if self.mode == "wander" and self.seconds is not None and now_s - self._t0 >= self.seconds - 1e-9:
            return self._end("done")
        if sonar_m is not None and sonar_m < self.lim.clear_m:
            return self._end("blocked")
        # the second look (LLD §25 A2) - after the stop rules, which hold in it too (review I2)
        if self.find and self.state == "second":
            return self._second_look(now_s, at, snap, cam_still)
        if (
            self.find
            and self._glimpses
            and self._may_look_again()
            and (b := self._new_glimpse(now_s, at)) is not None
        ):
            self._second = _SecondLook(b, self.state, now_s)
            self.state, self._second_s = "second", now_s
            return self._second_look(now_s, at, snap, cam_still)
        cost = self._costmap(snap)
        if self.state == "start":
            if self._start_t0 is None:
                self._start_t0, self._pan = now_s, 0.0
                return self._halt((0.0, self.lim.drive_tilt_deg))
            if now_s - self._start_t0 < START_STAND_S - 1e-9:
                return self._halt()
            self.state = "drive"
        planner = self.planner if snap.angle_trusted and snap.depth_ok else self.slow
        if self.point is not None and math.dist(at[:2], self.point) < POINT_SLOW_M:
            planner = self.slow
        if self.find and self.state == "drive" and self.candidates.pending() and now_s >= self._retry_s:
            c = self.candidates.next(at, self._costs_for(snap), self.routes)
            self._tried_now = True
            if c is None:
                self._retry_s = now_s + CANDIDATE_RETRY_S
            else:
                self._start_check(now_s, at, c)
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
        inp = SkillInput(at, snap, self._costs_for(snap), fix, sonar_m, map_ok, cam_still, True)
        out = self.chase.tick(now_s, inp)
        self.cmd = out.cmd
        if out.result in ("not_found", "lost"):  # it got away: roam on, a fresh chase for the next sighting
            if self._check is not None:  # the candidate it came from was chased and lost: ruled out (M1) ...
                self.candidates.rule_out(self._check.cand)
                self._check = None
            if (last := self.chase.last_seen) is not None:  # ... its last seen spot checked again from near,
                self._sighting(last)  # at most REOPEN_MAX times (§24 D)
            self.chase = ChaseSkill(self.planner, self.settings, search_when_lost=False)
            self._maybe_fix = None
            self.state = "drive"
            self._progress.reset()
            return self._halt((0.0, self.lim.drive_tilt_deg))
        if out.result is not None:
            return self._end(out.result)
        return out

    # ---- where to
    def _destination(self, now_s: float, at: XYTh, cost: CostMap) -> XY | None:
        return self.goal.destination(now_s, at, cost, self.routes)

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
            self.goal.drop()
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
            return self._halt((0.0, self.lim.drive_tilt_deg))
        cmd, cam = out
        if cam is not None:
            self._pan = cam[0]
        return self._halt(cam) if cmd == STOP else self._out(cmd, cam)

    # ---- look again at a candidate (find, §179, §189)
    def _start_check(self, now_s: float, at: XYTh, c: Candidate) -> None:
        far = max(0.0, math.dist(at[:2], c.spot) - CHECK_CLOSE_M)  # 10 s more per metre to drive (§190 I3)
        self._check, self.state = _Check(c, now_s, CHECK_MAX_S + CHECK_S_PER_M * far), "check"
        self._progress.reset()

    def _end_check(self, now_s: float, near: bool) -> SkillOut:
        """Not confirmed. Looked at from near: ruled out for the run (a remembered thing: gone). Couldn't get
        near (no way, no progress, out of time): pending, tried again later; after CHECK_TRIES ruled out, a
        remembered thing not marked gone (review I3). Search on."""
        if self._check is not None:
            c = self._check.cand
            if near:
                self.candidates.rule_out(c)
                if c.remembered is not None and self.memory is not None:
                    self.memory.mark_gone(c.remembered)
            else:
                c.tries += 1
                if c.tries >= CHECK_TRIES:
                    self.candidates.rule_out(c)
                self._retry_s = now_s + CANDIDATE_RETRY_S
        self._check, self.state, self._pan = None, "drive", 0.0
        self._progress.reset()
        return self._halt((0.0, self.lim.drive_tilt_deg))

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
        if c is None or now_s - c.t0 >= c.cap_s - 1e-9:
            return self._end_check(now_s, near=False)
        d = math.dist(at[:2], c.spot)
        b = bearing_deg(at, c.spot)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        if c.phase == "look":
            cam = None
            if not c.aimed:
                c.aimed, c.still_t0, self._pan = True, None, pan
                cam = (pan, self.lim.drive_tilt_deg)
            elif not cam_still:
                c.still_t0 = None
            elif c.still_t0 is None:
                c.still_t0 = now_s
            near_look = d <= CHECK_CLOSE_M + CHECK_NEAR_TOL_M  # hits count only from near (final review I1)
            if near_look and cam is None and cam_still and self._used:  # a new, still picture of the spot
                near = [p for p in self._maybes if math.dist(p, c.spot) <= SAME_SPOT_M]
                if near:
                    c.hits, c.last_maybe = c.hits + 1, near[-1]
                if c.hits >= CHECK_HITS and c.last_maybe is not None:  # seen twice from near: it is there ...
                    made = self._inside(TargetFix(-1, c.last_maybe[0], c.last_maybe[1], 0.0))
                    if made is not None:  # ... unless outside the play area: never chased (§196, review M2)
                        self._maybe_seen_s, self._maybe_fix = now_s, made
                        return self._chase(now_s, at, snap, made, sonar_m, True, cam_still)
                    c.hits, c.last_maybe = 0, None
            if c.still_t0 is not None and now_s - c.still_t0 >= CHECK_LOOK_S - 1e-9:
                if c.closer or d <= CHECK_CLOSE_M + CHECK_NEAR_TOL_M:
                    return self._end_check(now_s, near=True)
                c.phase = "closer"
            return self._halt(cam)
        if d <= CHECK_CLOSE_M + CHECK_NEAR_TOL_M:
            c.phase, c.closer, c.aimed = "look", True, False
            return self._halt()
        if (n := self._nearer(at, c.cand, d, snap)) is not None:  # the far one waits, pending, no try counted
            self._start_check(now_s, at, n)
            return self._halt()
        # the way: its own marks set aside, inside the play area (§189); the fan still keeps off every mark
        route = self.candidates.route(c.cand, at, self._costs_for(snap), self.routes)
        cost = self._costmap(snap)
        if route is None or not self._progress.ok(now_s, at):
            return self._end_check(now_s, near=False)
        obstacle = None if sonar_m is None else sonar_m + self.lim.sonar_x_m
        want = planner.choose(at, self.cmd, aim_point(route, (at[0], at[1])), obstacle, cost, far_m=math.inf)
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, self.lim.drive_tilt_deg)
        return self._halt(cam) if want == STOP else self._out(want, cam)

    def _nearer(self, at: XYTh, current: Candidate, d: float, snap: MapSnapshot) -> Candidate | None:
        """A pending candidate much nearer than the one being driven to (LLD §25 B): the nearest such, when
        there is a way to it (one outside the area or walled off would end its check at once: review)."""
        best: Candidate | None = None
        for p in self.candidates.pending():
            e = math.dist(at[:2], p.spot)
            nearer = p is not current and e < NEARER_FACTOR * d and d - e >= NEARER_BY_M - 1e-9
            if nearer and (best is None or e < math.dist(at[:2], best.spot)):
                best = p
        if best is None or self.candidates.route(best, at, self._costs_for(snap), self.routes) is None:
            return None
        return best

    # ---- the second look (LLD §25 A2)
    def _may_look_again(self) -> bool:
        """Driving, searching or driving to a check - not in the close look, the chase or a way out."""
        c = self._check
        return self.state == "drive" or (self.state == "check" and c is not None and c.phase == "closer")

    def _new_glimpse(self, now_s: float, at: XYTh) -> list[float] | None:
        """The newest glimpse not looked at already: its bearings to look at, or None."""
        if now_s - self._second_s < SECOND_LOOK_GAP_S - 1e-9:
            return None
        self._looked = [e for e in self._looked if now_s - e[2] < GLIMPSE_FORGET_S]
        for g in reversed(self._glimpses):
            mid = (g.lo_rad + g.hi_rad) / 2
            if self._known(g):
                continue
            if any(
                math.dist(at[:2], p) <= SAME_SPOT_M
                and abs(math.degrees(math.remainder(mid - b, math.tau))) <= GLIMPSE_SAME_DEG
                for p, b, _ in self._looked
            ):
                continue
            self._looked.append((at[:2], mid, now_s))
            if math.degrees(g.hi_rad - g.lo_rad) <= ONE_LOOK_DEG:
                return [mid]
            return [g.lo_rad, g.hi_rad]
        return None

    def _known(self, g: Glimpse) -> bool:
        """The glimpse points at the spot being checked, or at a candidate within GLIMPSE_KNOWN_M (pending or
        ruled out): that one, not something new - the camera following a spot in steps shows it moving
        (review I1)."""
        spots = [c.spot for c in self.candidates.pending()] + self.candidates.ruled_out()
        if self._check is not None:
            spots.append(self._check.spot)
        pad = math.radians(GLIMPSE_SAME_DEG)
        mid, half = (g.lo_rad + g.hi_rad) / 2, (g.hi_rad - g.lo_rad) / 2 + pad
        for p in spots:
            if math.dist(g.at, p) <= GLIMPSE_KNOWN_M:
                b = math.atan2(p[1] - g.at[1], p[0] - g.at[0])
                if abs(math.remainder(b - mid, math.tau)) <= half:
                    return True
        return False

    def _second_look(self, now_s: float, at: XYTh, snap: MapSnapshot, cam_still: bool) -> SkillOut:
        """Each bearing in turn: beyond SECOND_LOOK_PAN_DEG the tank turns toward it first (on the map's rule,
        like every turn; refused: the camera as far as it goes); then the camera on it and SECOND_LOOK_S of
        still pictures (SECOND_LOOK_MAX_S at most). The sightings meanwhile are maybes as ever. Then back."""
        sl = self._second
        assert sl is not None
        if sl.i >= len(sl.bearings):
            self._second, self.state, self._pan = None, sl.back, None
            if self._check is not None:  # the check's time doesn't run while it looked elsewhere
                self._check.t0 += now_s - sl.t0
            self._progress.reset()
            return self._halt()
        rel = math.degrees(math.remainder(sl.bearings[sl.i] - at[2], math.tau))  # + = left
        if sl.look_t0 is None and abs(rel) > SECOND_LOOK_PAN_DEG:
            if sl.turn_t0 is None:
                sl.turn_t0 = now_s
            left = rel > 0
            turning = now_s - sl.turn_t0 < SECOND_TURN_S - 1e-9  # a slipping track: no turn for ever (I2)
            if turning and self.planner.can_spin(
                at, self._costmap(snap), left, abs(rel) - SECOND_LOOK_PAN_DEG
            ):
                return self._out(Command(0.0, math.copysign(self.lim.spin_dps, rel)))
        if sl.look_t0 is None:
            pan = max(-SECOND_LOOK_PAN_DEG, min(SECOND_LOOK_PAN_DEG, -rel))
            sl.look_t0, sl.still_t0, self._pan = now_s, None, pan
            return self._halt((pan, self.lim.drive_tilt_deg))
        if not cam_still:
            sl.still_t0 = None
        elif sl.still_t0 is None:
            sl.still_t0 = now_s
        looked = sl.still_t0 is not None and now_s - sl.still_t0 >= SECOND_LOOK_S - 1e-9
        if looked or now_s - sl.look_t0 >= SECOND_LOOK_MAX_S - 1e-9:
            sl.i, sl.turn_t0, sl.look_t0, sl.still_t0 = sl.i + 1, None, None, None
            return self._second_look(now_s, at, snap, cam_still)
        return self._halt()

    # ---- driving
    def _drive(
        self, now_s: float, at: XYTh, cost: CostMap, planner: Planner, sonar_m: float | None
    ) -> SkillOut:
        route = self._plan(now_s, at, cost)
        # setxy: no way for NO_ROUTE_S ends it (the tank stands and waits meanwhile)
        if self.point is not None:
            if route is not None:
                self._no_route_t0 = None
            else:
                if self._no_route_t0 is None:
                    self._no_route_t0 = now_s
                if now_s - self._no_route_t0 >= NO_ROUTE_S - 1e-9:
                    return self._end(Result.NO_WAY, "no route to the point")
                return self._halt()
        if route is None:
            if self.mode == "explore" and self.goal.done:
                return self._end("done")
            if self.find and self.goal.done:  # every spot looked at, never seen (§188)
                if self.candidates.pending() and not self._tried_now:
                    return self._halt()  # a candidate waits for its retry: not_found only after (review M3)
                if self._costs is not None and self.candidates.pending():  # every one routed once (Z7 I1)
                    c = self.candidates.next(at, self._costs, self.routes, limit=None)
                    if c is not None:
                        self._start_check(now_s, at, c)
                        return self._halt()
                outside = self._outside_m
                why = "" if outside is None else f"seen outside the play area, {outside:.1f} m from the start"
                return self._end("outside_area" if outside is not None else "not_found", why)
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
            cam = (pan, self.lim.drive_tilt_deg)
        return self._halt(cam) if want == STOP else self._out(want, cam)
