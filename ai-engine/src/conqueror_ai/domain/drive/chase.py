"""Chase the ball (AI engine LLD §17.6, spec 8 §4.2): looking, approaching, arrived or lost."""

from __future__ import annotations

import math
from dataclasses import replace

from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.goal import TargetFix, echo_is_target
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.recovery import ProgressWatch, Recovery, TurnMeter
from conqueror_ai.domain.drive.route import RoutePlanner, aim_point
from conqueror_ai.domain.drive.skill import Result, SkillInput, SkillOut
from conqueror_ai.domain.room_map import MapSnapshot

SWEEP_PANS = (0.0, -45.0, 45.0, -90.0, 90.0)  # pan + = right (cam_set); straight ahead first
SWEEP_LOOK_S = 0.8  # still pictures at each position (the map skips pictures while the camera moves)
SWEEP_MAX_S = 2.5  # at most this long at one position (a camera that never settles)
LOST_AFTER_S = 3.0  # unseen this long while the camera looks at the thing's spot: lost (design-notes §187)
VIEW_DEG = 30.0  # the thing's spot within this of where the camera points counts as looked at (lens +-41)
ARRIVE_BEARING_DEG = 10.0
PAN_LIMIT_DEG = 60.0
PAN_STEP_DEG = 8.0  # the camera follows the target in steps this big, not constantly
SEARCH_TURN_DEG = 180.0  # after the first sweep: half a turn on the spot, then sweep what was behind
SEARCH_AIM_DEG = 170.0  # curving round instead (the turn refused): aim 1 m off this far left, at most
MAX_TICK_S = 0.3  # a longer gap between ticks counts as this (no jump in the ramps after a stall)
STALE_GIVE_UP_S = 2.0  # a stale map stops the tank at once; this long ends the run (design-notes §157.2)
TARGET_R_M = 0.12  # on the room map, the target's own marks (the ball is 12 cm) don't count as an obstacle


class ChaseSkill:
    def __init__(
        self, planner: Planner, cost: CostSettings | None = None, search_when_lost: bool = True
    ) -> None:
        """search_when_lost: a lost thing is looked for where the tank stands (sweep, half a turn, sweep);
        False inside find - it ends `lost` at once, and find checks the last seen spot from near (§25 C)."""
        self.planner = planner
        self.search_when_lost = search_when_lost
        self.settings = cost or CostSettings()
        self.slow = Planner(
            replace(planner.lim, v_cap_cms=min(planner.lim.v_cap_cms, planner.lim.slow_cms)), planner.w
        )
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
        self.recovery = Recovery(
            planner.lim, planner.foot, planner.lim.drive_tilt_deg
        )  # on the map (Plan Z6)
        self._recovering = False
        self._progress = ProgressWatch(self.recovery, self.settings.stuck_s)  # on the map
        self._spin_turned = TurnMeter()  # the search turn
        self._costs: CostMaps | None = None  # this tick's cost maps (the play area is in them)
        self._own: tuple[MapSnapshot, CostMaps] | None = None  # built here when not handed in
        self._target: tuple[float, float] | None = None  # the thing approached last (the viewer)
        self._cam_pan: float | None = None  # where the camera was last sent (None: not by this skill)
        self._seen_at: float | None = None  # when the target was last seen
        self._away_s = 0.0  # since then, time the camera didn't look at its spot (§187)

    def _out(
        self, want: Command, cam: tuple[float, float] | None = None, result: Result | None = None
    ) -> SkillOut:
        self.cmd = ramp(self.cmd, want, self._dt, self.lim)
        return SkillOut(self.cmd, cam, result)

    def _end(self, result: Result) -> SkillOut:
        self.state = "arrived" if result == "done" else "lost"
        self.cmd = STOP
        return SkillOut(STOP, None, result)

    def cancel(self) -> SkillOut:
        return self._end(Result.CANCELLED)

    @property
    def last_seen(self) -> tuple[float, float] | None:
        """Where the thing was last approached (a lost chase leaves a candidate there, LLD §24 D)."""
        return self._target

    def debug(self) -> dict[str, object]:
        return {"state": self.state, "recovering": self._recovering, "target": self._target}

    def _costs_for(self, snap: MapSnapshot) -> CostMaps:
        """This tick's cost maps: those handed in (roam's, with its play area), else one set per snapshot."""
        if self._costs is not None and self._costs.snap is snap:
            return self._costs
        if self._own is None or self._own[0] is not snap:
            self._own = (snap, CostMaps(snap, self.settings, None))
        return self._own[1]

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        """One tick (LLD §19.4): the pose, the target (`inp.fix`), the echo, the flags and the room map with
        its cost maps (the play area in them). Every skill drives on the map: with none it stops, as when
        stale (no GOTO, design-notes §191)."""
        at, fix, sonar_m, snap = inp.at, inp.fix, inp.sonar_m, inp.snap
        if self.state in ("arrived", "lost"):
            return SkillOut(STOP)
        if self._last_s is not None:
            self._dt = min(max(now_s - self._last_s, 0.0), MAX_TICK_S)
        self._last_s = now_s
        self._cam_still = inp.cam_still
        self._costs = inp.costs
        if not inp.map_ok or at is None or snap is None:  # it can't see: stop at once, on when fresh
            if self._stale_t0 is None:
                self._stale_t0 = now_s
            if now_s - self._stale_t0 >= STALE_GIVE_UP_S - 1e-9:
                return self._end(Result.STALE)
            self.cmd = STOP
            return SkillOut(STOP)
        self._stale_t0 = None
        if self.state == "start":
            self.state, self._t0 = ("approaching", now_s) if fix is not None else ("sweep", now_s)
            self._pan, self._i = None, 0
        if self.state in ("sweep", "spin") and fix is not None and fix.seen_age_s < LOST_AFTER_S:
            self.state = "approaching"  # found: straight into the approach, no standing still in between
        if self.state in ("sweep", "spin") and sonar_m is not None and sonar_m < self.lim.clear_m:
            return self._end(Result.BLOCKED)  # nothing sought in view: an echo this near is in the way (I1)
        if self.state == "sweep":
            out = self._sweep(now_s)
        elif self.state == "spin":
            out = self._spin(now_s, at, snap, sonar_m)
        else:
            self._watch(now_s, at, fix)
            out = self._approach(now_s, at, fix, sonar_m, snap)
        if out.cam is not None:
            self._cam_pan = out.cam[0]
        return out

    def _watch(self, now_s: float, at: XYTh, fix: TargetFix | None) -> None:
        """Time since the target was last seen that the camera spent looking elsewhere (or moving): it
        doesn't count toward lost - not seeing a thing it isn't looking at says nothing (§187)."""
        if fix is None:
            return
        seen_at = now_s - fix.seen_age_s
        if self._seen_at is None or seen_at > self._seen_at + 1e-6:
            self._seen_at, self._away_s = seen_at, 0.0
            return
        b = bearing_deg(at, (fix.x_m, fix.y_m))
        if not self._cam_still or (self._cam_pan is not None and abs(b + self._cam_pan) > VIEW_DEG):
            self._away_s += self._dt

    def _lost(self, fix: TargetFix) -> bool:
        return fix.seen_age_s - self._away_s > LOST_AFTER_S

    def _sweep(self, now_s: float) -> SkillOut:
        """The camera to each pan in turn; at each, SWEEP_LOOK_S of still pictures (SWEEP_MAX_S at most)."""
        if self._i >= len(SWEEP_PANS):
            if self._swept >= 1:
                return self._end(Result.NOT_FOUND)
            self._swept += 1
            self.state, self._t0, self._pan = "spin", now_s, 0.0
            return self._out(STOP, (0.0, self.lim.drive_tilt_deg))
        pan = SWEEP_PANS[self._i]
        if self._pan != pan:
            self._pan, self._pos_t0, self._look_t0 = pan, now_s, None
            return self._out(STOP, (pan, self.lim.drive_tilt_deg))
        if not self._cam_still:
            self._look_t0 = None
        elif self._look_t0 is None:
            self._look_t0 = now_s
        looked = self._look_t0 is not None and now_s - self._look_t0 >= SWEEP_LOOK_S - 1e-9
        if looked or now_s - self._pos_t0 >= SWEEP_MAX_S - 1e-9:
            self._i += 1
            return self._sweep(now_s)
        return self._out(STOP)

    def _spin(self, now_s: float, at: XYTh, snap: MapSnapshot, sonar_m: float | None = None) -> SkillOut:
        """Half a turn, measured from the pose. The map can't use pictures taken while spinning (faster than
        its 45 deg/s gate), so the tank then stands and sweeps the camera over what was behind it. On the map
        the turn on the spot is checked like every move (review C1); over floor never seen it curves round on
        the forward curves instead (§176); with none allowed, it turns on the spot anyway if only unseen floor
        stood in the way (the last resort), else the search sweeps from here."""
        rest = SEARCH_TURN_DEG - self._spin_turned.update(at[2])
        cost = self._costs_for(snap).base()
        want = Command(0.0, self.lim.spin_dps)
        if rest > 1e-9 and not self.planner.can_spin(at, cost, True, max(rest, 1.0)):
            # refused (near something, or over floor never seen - §176): curve round on the forward curves
            a = at[2] + math.radians(min(rest, SEARCH_AIM_DEG))
            ahead = None if sonar_m is None else sonar_m + self.lim.sonar_x_m  # the curve heeds the echo too
            want = self.planner.choose(at, self.cmd, (at[0] + math.cos(a), at[1] + math.sin(a)), ahead, cost)
            if want == STOP and self.planner.can_spin(at, cost, True, max(rest, 1.0), seen=False):
                want = Command(0.0, self.lim.spin_dps)  # no curve allowed: the last resort, as the ways out
        if rest <= 1e-9 or want == STOP:
            self.state, self._t0, self._pan, self._i = "sweep", now_s, None, 0
            self._spin_turned.reset()
            self.cmd = STOP
            return self._sweep(now_s)
        return self._out(want)

    def _approach(
        self, now_s: float, at: XYTh, fix: TargetFix | None, sonar_m: float | None, snap: MapSnapshot
    ) -> SkillOut:
        lim = self.lim
        if fix is None or self._lost(fix):
            if not self.search_when_lost:
                return self._end(Result.LOST)
            self.state, self._t0, self._pan, self._i = "sweep", now_s, None, 0
            self._recovering, self._blocked_t0 = False, None  # review I2
            self._progress.reset()
            self.cmd = STOP
            return self._sweep(now_s)
        target = (fix.x_m, fix.y_m)
        self._target = target
        b = bearing_deg(at, target)
        d_lens = self.planner.lens_distance(at, target)
        obstacle: float | None = None
        if sonar_m is not None:
            ahead = sonar_m + lim.sonar_x_m
            if not echo_is_target(ahead - lim.cam_x_m, d_lens, b):
                if sonar_m < lim.clear_m:
                    return self._end(Result.BLOCKED)
                obstacle = ahead
        fan = self._costs_for(snap).ignoring(target[0], target[1], TARGET_R_M)
        if (ij := fan.cell_of(*target)) is not None and fan.lethal[ij]:
            return self._end(Result.BLOCKED)  # where it may not go even with its own marks set aside (§174)
        if self._recovering:  # stuck on the map: the standard ways out (Plan Z6)
            return self._recover(now_s, at, fan, target)
        if d_lens <= lim.stop_range_m + 0.05:
            if abs(b) <= ARRIVE_BEARING_DEG:
                self.cmd = ramp(self.cmd, STOP, self._dt, lim)
                if self.cmd == STOP:
                    return self._end(Result.DONE)
                return SkillOut(self.cmd)
            if not self.planner.can_spin(
                at, fan, b > 0, abs(b), seen=False
            ):  # turning to face it would scrape (review C1); the alternative is stopping, so not seen=True
                self.cmd = STOP
                return self._end(Result.DONE)
            # close enough but off to the side: driving on would pass it, so turn on the spot to face it
            return self._out(Command(0.0, math.copysign(lim.spin_dps, b)))
        # round obstacles on the room map: steer along the route, slow down for the thing itself
        planner = self.planner if snap.angle_trusted and snap.depth_ok else self.slow
        reach = lim.stop_range_m + lim.cam_x_m  # the chase stops this far from the thing's centre anyway
        route = self.routes.route(fan, (at[0], at[1]), target, goal_ok=True, near_m=reach)
        want = (
            STOP
            if route is None
            else planner.choose(at, self.cmd, aim_point(route, (at[0], at[1])), obstacle, fan, d_lens)
        )
        give_up = self.settings.stuck_s
        if want == STOP:  # every curve blocked (design-notes §157.5), or no route
            if self._blocked_t0 is None:
                self._blocked_t0 = now_s
        else:
            self._blocked_t0 = None
        blocked = self._blocked_t0 is not None and now_s - self._blocked_t0 >= give_up - 1e-9
        if blocked or not self._progress.ok(now_s, at):  # stuck on the map (§18.13.4)
            self._blocked_t0, self._recovering = None, True
            self._progress.reset()
            self.recovery.start(now_s, at)
            self.cmd = STOP
            return SkillOut(STOP)
        pan = max(-PAN_LIMIT_DEG, min(PAN_LIMIT_DEG, -b))
        cam = None
        if self._pan is None or abs(pan - self._pan) > PAN_STEP_DEG:
            self._pan = pan
            cam = (pan, self.lim.drive_tilt_deg)
        if (
            want == STOP
        ):  # blocked: stop at once, not by the speed ramp (Plan Z5: the ramp coasted into things)
            self.cmd = STOP
            return SkillOut(STOP, cam)
        return self._out(want, cam)

    def _recover(self, now_s: float, at: XYTh, cost: CostMap, toward: tuple[float, float]) -> SkillOut:
        out = self.recovery.tick(now_s, at, cost, toward)
        if self.recovery.step == "blocked":
            return self._end(Result.BLOCKED)
        if out is None:  # a step finished: approach again
            self._recovering, self._pan = False, None
            self.cmd = STOP
            return SkillOut(STOP)
        cmd, cam = out
        if cmd == STOP:
            self.cmd = STOP
            return SkillOut(STOP, cam)
        return self._out(cmd, cam)
