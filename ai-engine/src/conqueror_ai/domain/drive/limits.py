"""What the tank can do smoothly (spec 8 §2, design-notes §69-§71, §114) and slice 1's caps
(AI engine LLD §17)."""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class Limits:
    v_min_cms: float = 18.0  # the slowest smooth straight speed (the Uno's dead-band lift)
    v_cruise_cms: float = 25.0
    v_cap_cms: float = 25.0  # slice 1's cap (spec 8 §5)
    track_m: float = 0.19  # the tracks' effective width
    inner_min_cms: float = 17.7  # the inner track can't run slower than this
    r_floor_m: float = 0.31  # no arc under ~31 cm is smooth at any speed (spec 02 Part H)
    spin_dps: float = 65.0  # the slowest smooth spin in place
    dv_cms_per_s: float = 10.0  # the speed ramp
    dw_dps_per_s: float = 60.0  # the turn-rate ramp
    stop_range_m: float = 0.40  # from the lens to the ball's centre at arrival
    slow_from_m: float = 0.80  # the speed falls to v_min from here
    # keep the tank's front (the sonar) this far from an echo that isn't the target; nearer ends the run
    # (owner: 10 cm, design-notes §159; slice 1 had 30 cm). The Uno's 15 cm brake stays underneath.
    clear_m: float = 0.10
    cam_x_m: float = 0.085  # the lens ahead of the tank's centre
    sonar_x_m: float = 0.13  # the sonar ahead of the tank's centre
    horizon_s: float = 1.5  # how far ahead a curve is rolled out
    # the body round the pose's point (the tracks included); a turn on the spot sweeps its farthest corner
    # (review C1). The owner's tape from the turning centre: front 5 in, rear 3.5 in, width 9 in, each rounded
    # up to the half centimetre (design-notes §163).
    body_front_m: float = 0.13
    body_rear_m: float = 0.09
    body_half_w_m: float = 0.115
    dt_s: float = 0.1  # one tick

    def r_min_m(self, v_cms: float) -> float:
        """The tightest smooth arc at this speed: the inner track at its lowest speed, never under r_floor."""
        if v_cms <= self.inner_min_cms:
            return math.inf
        return max(self.r_floor_m, self.track_m / 2 * v_cms / (v_cms - self.inner_min_cms))

    def top_speed(self, d_lens_m: float) -> float:
        """The fastest speed for the target's distance: cruise far away, falling to v_min near the stop."""
        cruise = min(self.v_cruise_cms, self.v_cap_cms)
        near = self.stop_range_m + 0.10
        if d_lens_m >= self.slow_from_m:
            return cruise
        if d_lens_m <= near:
            return self.v_min_cms
        f = (d_lens_m - near) / (self.slow_from_m - near)
        return self.v_min_cms + f * (cruise - self.v_min_cms)

    @property
    def sweep_r_m(self) -> float:
        """The farthest corner of the body from the pose's point: what a turn on the spot sweeps."""
        return math.hypot(max(self.body_front_m, self.body_rear_m), self.body_half_w_m)
