"""The planner (AI engine LLD §17.4, spec 8 §3): a fan of curves the tank can drive smoothly, each rolled out
1.5 s and scored on progress, clearance and smoothness; the best one wins."""

from __future__ import annotations

import math
from dataclasses import dataclass

from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, roll_out
from conqueror_ai.domain.drive.limits import Limits

RADII_M = (1.5, 0.8, 0.5)  # plus straight on and the tightest smooth arc
SPIN_FROM_DEG = 70.0  # a spin in place is offered only when the target is this far off
SPIN_UNTIL_DEG = 8.0  # once spinning, keep spinning until the target is this near straight ahead


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

    def lens_distance(self, at: XYTh, target: tuple[float, float]) -> float:
        return math.hypot(target[0] - at[0], target[1] - at[1]) - self.lim.cam_x_m

    def fan(self, at: XYTh, target: tuple[float, float], now: Command = STOP) -> list[Command]:
        lim = self.lim
        b = bearing_deg(at, target)
        spin = Command(0.0, math.copysign(lim.spin_dps, b))
        if now.v_cms == 0 and abs(now.w_dps) >= lim.spin_dps and abs(b) > SPIN_UNTIL_DEG:
            return [spin]  # finish the turn on the spot: no spin-drive-spin stutter
        top = lim.top_speed(self.lens_distance(at, target))
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
        if self.inside_turn(at, target):
            return [STOP, spin]  # no curve can reach it: turn on the spot toward it first
        if abs(b) > SPIN_FROM_DEG:
            out.append(spin)
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
        self, at: XYTh, now: Command, want: Command, target: tuple[float, float], obstacle_m: float | None
    ) -> float:
        lim, w = self.lim, self.w
        path = roll_out(at, now, want, lim)
        end = path[-1]
        d0 = max(self.lens_distance(at, target), lim.stop_range_m)
        d1 = max(self.lens_distance(end, target), lim.stop_range_m)
        s = w.progress * (d0 - d1) / 0.3 - w.heading * abs(bearing_deg(end, target)) / 90.0
        if now.v_cms > 0:  # steering changes while driving cost (speed changes are the ramp's job)
            s -= w.smooth * abs(want.w_dps - now.w_dps) / 30.0
        if obstacle_m is not None and want.v_cms > 0:
            ox = at[0] + obstacle_m * math.cos(at[2])
            oy = at[1] + obstacle_m * math.sin(at[2])
            # keep clear_m between the tank's front (where the sonar is) and the echo, as the chase does
            fx = lim.sonar_x_m
            if any(
                math.hypot(px + fx * math.cos(th) - ox, py + fx * math.sin(th) - oy) < lim.clear_m
                for px, py, th in path
            ):
                s += w.blocked
        return s

    def choose(
        self, at: XYTh, now: Command, target: tuple[float, float], obstacle_m: float | None = None
    ) -> Command:
        """The best curve to the target; obstacle_m: an echo straight ahead (from the tank's centre) that is
        not the target."""
        moving = [c for c in self.fan(at, target, now) if c != STOP]
        scored = [(self.score(at, now, c, target, obstacle_m), c) for c in moving]
        best_score, best = max(scored, key=lambda sc: sc[0])
        return STOP if best_score <= self.w.blocked / 2 else best  # stop only when every curve is blocked
