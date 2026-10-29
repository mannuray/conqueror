"""The planner (AI engine LLD §17.4, spec 8 §3): a fan of curves the tank can drive smoothly, each rolled out
1.5 s and scored on progress, clearance and smoothness; the best one wins."""

from __future__ import annotations

import math
from dataclasses import dataclass

from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, roll_out
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.route import ROBOT_R_M

RADII_M = (1.5, 0.8, 0.5)  # plus straight on and the tightest smooth arc
# More than this far off, the tank turns on the spot first. (Floor chase 5: the tightest arcs the inner track
# allows are 30-50 cm, so a target 40 deg off at 80 cm was swept past by a curve; design-notes §157.)
SPIN_FROM_DEG = 35.0
# Once spinning, keep spinning until the target is this near straight ahead. The tank coasts on after the stop
# and the pose lags: floor chase 6 turned 55 deg for a ball 40 deg off with 8 (design-notes §157.2); the
# drive's arc aims the rest.
SPIN_UNTIL_DEG = 15.0
# On the room map a curve may not take the tank's centre this near an obstacle (AI engine LLD §18.6) - unless
# it is that near already and the curve doesn't bring it nearer (so a tank by a wall can move away).
MAP_CLEAR_M = ROBOT_R_M - 0.03
FRONT_CLEAR_M = 0.05  # and the tank's front (the sonar, 13 cm ahead of the centre) keeps this much (Plan Z5)
SPIN_MARGIN_M = 0.02  # a turn on the spot needs the body's swept circle + this clear (review C1)


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

    def can_spin(self, at: XYTh, clear: ClearanceMap | None) -> bool:
        """Whether turning on the spot here keeps the body's corners clear on the room map (no map: yes)."""
        return clear is None or clear.dist_m(at[0], at[1]) >= self.lim.sweep_r_m + SPIN_MARGIN_M

    def lens_distance(self, at: XYTh, target: tuple[float, float]) -> float:
        return math.hypot(target[0] - at[0], target[1] - at[1]) - self.lim.cam_x_m

    def fan(
        self,
        at: XYTh,
        target: tuple[float, float],
        now: Command = STOP,
        far_m: float | None = None,
        clear: ClearanceMap | None = None,
    ) -> list[Command]:
        """far_m: how far the way's end still is (lens distance) when the target is only an aim point on a
        route (Plan Z5): the speed follows the end, the steering the aim point."""
        lim = self.lim
        b = bearing_deg(at, target)
        spin = Command(0.0, math.copysign(lim.spin_dps, b))
        spin_ok = self.can_spin(at, clear)
        if now.v_cms == 0 and abs(now.w_dps) >= lim.spin_dps and abs(b) > SPIN_UNTIL_DEG and spin_ok:
            return [spin]  # finish the turn on the spot: no spin-drive-spin stutter
        top = lim.top_speed(self.lens_distance(at, target) if far_m is None else far_m)
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
        if spin_ok and (abs(b) > SPIN_FROM_DEG or (self.inside_turn(at, target) and abs(b) > SPIN_UNTIL_DEG)):
            # well off to one side, or no curve reaches it: turn on the spot first. Nearly ahead, straight on
            # passes close enough - at the slowest speed no curve reaches anything off-centre, and turning to
            # it exactly overshot back and forth for ever (Plan Z5)
            return [STOP, spin]
        # too near something to turn on the spot (review C1): the forward curves, to drive out first
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
        self,
        at: XYTh,
        now: Command,
        want: Command,
        target: tuple[float, float],
        obstacle_m: float | None,
        clear: ClearanceMap | None = None,
        far_m: float | None = None,
    ) -> float:
        lim, w = self.lim, self.w
        path = roll_out(at, now, want, lim)
        end = path[-1]
        if far_m is None:
            d0 = max(self.lens_distance(at, target), lim.stop_range_m)
            d1 = max(self.lens_distance(end, target), lim.stop_range_m)
            gain = d0 - d1
        else:  # an aim point on a route: the way goes on past it, so progress is distance made toward it
            a = math.atan2(target[1] - at[1], target[0] - at[0])
            gain = (end[0] - at[0]) * math.cos(a) + (end[1] - at[1]) * math.sin(a)
        s = w.progress * gain / 0.3 - w.heading * abs(bearing_deg(end, target)) / 90.0
        if now.v_cms > 0:  # steering changes while driving cost (speed changes are the ramp's job)
            s -= w.smooth * abs(want.w_dps - now.w_dps) / 30.0
        moves = want.v_cms > 0 or now.v_cms > 0  # a turn on the spot from driving coasts on first
        if obstacle_m is not None and moves:
            ox = at[0] + obstacle_m * math.cos(at[2])
            oy = at[1] + obstacle_m * math.sin(at[2])
            # keep clear_m between the tank's front (where the sonar is) and the echo, as the chase does
            fx = lim.sonar_x_m
            if any(
                math.hypot(px + fx * math.cos(th) - ox, py + fx * math.sin(th) - oy) < lim.clear_m
                for px, py, th in path
            ):
                s += w.blocked
        if clear is not None and moves:
            fx = lim.sonar_x_m

            def front(px: float, py: float, th: float) -> float:
                return clear.dist_m(px + fx * math.cos(th), py + fx * math.sin(th))

            limit = min(MAP_CLEAR_M, clear.dist_m(at[0], at[1]) - 0.01)
            front_limit = min(FRONT_CLEAR_M, front(*at) - 0.01)
            if any(clear.dist_m(px, py) < limit or front(px, py, th) < front_limit for px, py, th in path):
                s += w.blocked
        if clear is not None and want.v_cms == 0 and want.w_dps != 0 and not self.can_spin(at, clear):
            s += w.blocked  # a turn on the spot would sweep a corner into something
        return s

    def choose(
        self,
        at: XYTh,
        now: Command,
        target: tuple[float, float],
        obstacle_m: float | None = None,
        clear: ClearanceMap | None = None,
        far_m: float | None = None,
    ) -> Command:
        """The best curve to the target; obstacle_m: an echo straight ahead (from the tank's centre) that is
        not the target; clear: the room map's clearance (Plan Z5), None for slice 1's sonar-only check; far_m:
        see fan(); within slow_from_m of the end it is slice 1's approach again (the aim is the end)."""
        if far_m is not None and far_m < self.lim.slow_from_m:
            far_m = None
        moving = [c for c in self.fan(at, target, now, far_m, clear) if c != STOP]
        scored = [(self.score(at, now, c, target, obstacle_m, clear, far_m), c) for c in moving]
        best_score, best = max(scored, key=lambda sc: sc[0])
        return STOP if best_score <= self.w.blocked / 2 else best  # stop only when every curve is blocked
