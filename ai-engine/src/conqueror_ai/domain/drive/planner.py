"""The planner (AI engine LLD §17.4, spec 8 §3): a fan of curves the tank can drive smoothly, each rolled out
1.5 s and scored on progress, clearance and smoothness; the best one wins."""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, roll_out
from conqueror_ai.domain.drive.limits import Limits

RADII_M = (1.5, 0.8, 0.5)  # plus straight on and the tightest smooth arc
# More than this far off, the tank turns on the spot first. (Floor chase 5: the tightest arcs the inner track
# allows are 30-50 cm, so a target 40 deg off at 80 cm was swept past by a curve; design-notes §157.)
SPIN_FROM_DEG = 35.0
# Once spinning, keep spinning until the target is this near straight ahead. The tank coasts on after the stop
# and the pose lags: floor chase 6 turned 55 deg for a ball 40 deg off with 8 (design-notes §157.2); the
# drive's arc aims the rest.
SPIN_UNTIL_DEG = 15.0
# On the room map the body's outline (Limits' front / rear / half-width, design-notes §163) is rolled out
# along every move and kept this far from anything: the front edge FRONT_CLEAR_M, the rest BODY_CLEAR_M. A
# tank already nearer may still move as long as no part of it gets nearer (design-notes §164; floor run 2
# stood 13 s beside the bed when a whole turning circle was asked for).
FRONT_CLEAR_M = 0.05
BODY_CLEAR_M = 0.02
OUTLINE_STEP_M = 0.025  # outline points this far apart (the map's cells are 5 cm)
SWEEP_STEP_DEG = 5.0  # a move is checked at poses at most this far turned ...
SWEEP_STEP_M = 0.025  # ... and this far moved apart
SPIN_CHECK_DEG = 90.0  # a turn on the spot is checked this far round (the look's step)


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
        self._outline = _outline(lim)

    def can_spin(
        self, at: XYTh, clear: ClearanceMap | None, left: bool = True, deg: float = SPIN_CHECK_DEG
    ) -> bool:
        """Whether turning on the spot this way (deg round) keeps the body clear on the room map (no map:
        yes)."""
        if clear is None:
            return True
        sign = 1.0 if left else -1.0
        steps = max(1, math.ceil(deg / SWEEP_STEP_DEG))
        poses = [(at[0], at[1], at[2] + sign * math.radians(deg * k / steps)) for k in range(1, steps + 1)]
        return self.body_ok(at, poses, clear)

    def body_ok(self, at: XYTh, poses: list[XYTh], clear: ClearanceMap) -> bool:
        """The body's outline along these poses (from at) on the room map: nowhere within its margins - or,
        where it already is, no deeper in than at the start (the sum over the outline of how far each point
        is inside its margin never grows: turning away passes, turning or driving further in doesn't). On the
        map as given and on its marks alone (grown unseen space round the tank reads 0 everywhere)."""
        pts, margin = self._outline
        for marks_only in (False, True):
            start = self._inside(at, pts, margin, clear, marks_only)
            if any(self._inside(p, pts, margin, clear, marks_only) > start + 1e-9 for p in _dense(at, poses)):
                return False
        return True

    @staticmethod
    def _inside(p: XYTh, pts: FloatArray, margin: FloatArray, clear: ClearanceMap, marks_only: bool) -> float:
        c, s = math.cos(p[2]), math.sin(p[2])
        world = np.stack([p[0] + pts[:, 0] * c - pts[:, 1] * s, p[1] + pts[:, 0] * s + pts[:, 1] * c], axis=1)
        return float(np.maximum(margin - clear.dist_many(world, marks_only), 0.0).sum())

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
        spin_ok = self.can_spin(at, clear, left=b >= 0)
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
        if clear is not None and moves and not self.body_ok(at, path, clear):
            s += w.blocked
        if (
            clear is not None
            and want.v_cms == 0
            and want.w_dps != 0
            and not self.can_spin(at, clear, want.w_dps > 0)
        ):
            s += w.blocked  # a turn on the spot would sweep the body into something
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


def _outline(lim: Limits) -> tuple[FloatArray, FloatArray]:
    """Outline points in the tank's frame (x ahead, y left) and each one's margin (the front edge more)."""
    f, r, w = lim.body_front_m, lim.body_rear_m, lim.body_half_w_m
    ys = np.linspace(-w, w, max(2, math.ceil(2 * w / OUTLINE_STEP_M) + 1))
    xs = np.linspace(-r, f, max(2, math.ceil((f + r) / OUTLINE_STEP_M) + 1))[1:-1]
    front = np.stack([np.full_like(ys, f), ys], axis=1)
    rear = np.stack([np.full_like(ys, -r), ys], axis=1)
    sides = np.concatenate([np.stack([xs, np.full_like(xs, w)], 1), np.stack([xs, np.full_like(xs, -w)], 1)])
    margin = np.concatenate(
        [np.full(len(front), FRONT_CLEAR_M), np.full(len(rear) + len(sides), BODY_CLEAR_M)]
    )
    return np.concatenate([front, rear, sides]).astype(np.float64), margin.astype(np.float64)


def _dense(at: XYTh, poses: list[XYTh]) -> list[XYTh]:
    """The poses with others between, so none is more than SWEEP_STEP_DEG / SWEEP_STEP_M from the last."""
    out: list[XYTh] = []
    prev = at
    for p in poses:
        n = max(
            1,
            math.ceil(
                max(
                    abs(math.degrees(p[2] - prev[2])) / SWEEP_STEP_DEG,
                    math.dist(p[:2], prev[:2]) / SWEEP_STEP_M,
                )
            ),
        )
        out += [
            (
                prev[0] + (p[0] - prev[0]) * k / n,
                prev[1] + (p[1] - prev[1]) * k / n,
                prev[2] + (p[2] - prev[2]) * k / n,
            )
            for k in range(1, n + 1)
        ]
        prev = p
    return out
