"""One body rule (AI engine LLD §18.13.2, Plan Z6): the outline of the tank's rectangle (Limits, design-notes
§163), rolled out along a move. No outline point may come nearer than the keep-out to a known obstacle, and a
point already nearer may not come any nearer; everything else about nearness is cost (§18.13.1)."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.limits import Limits

OUTLINE_STEP_M = 0.025  # outline points this far apart (the map's cells are 5 cm)
SWEEP_STEP_DEG = 5.0  # a move is checked at poses at most this far turned ...
SWEEP_STEP_M = 0.025  # ... and this far moved apart


class Footprint:
    def __init__(self, lim: Limits) -> None:
        f, r, w = lim.body_front_m, lim.body_rear_m, lim.body_half_w_m
        ys = np.linspace(-w, w, max(2, math.ceil(2 * w / OUTLINE_STEP_M) + 1))
        xs = np.linspace(-r, f, max(2, math.ceil((f + r) / OUTLINE_STEP_M) + 1))[1:-1]
        edges = [np.stack([np.full_like(ys, f), ys], 1), np.stack([np.full_like(ys, -r), ys], 1)]
        sides = [np.stack([xs, np.full_like(xs, w)], 1), np.stack([xs, np.full_like(xs, -w)], 1)]
        self.outline: FloatArray = np.concatenate(edges + sides).astype(
            np.float64
        )  # tank frame: x ahead, y left

    def world(self, p: XYTh) -> FloatArray:
        c, s = math.cos(p[2]), math.sin(p[2])
        o = self.outline
        return np.stack([p[0] + o[:, 0] * c - o[:, 1] * s, p[1] + o[:, 0] * s + o[:, 1] * c], axis=1)

    @staticmethod
    def dense(at: XYTh, poses: list[XYTh]) -> list[XYTh]:
        """The poses with others between, none more than SWEEP_STEP_DEG / SWEEP_STEP_M from the last."""
        out: list[XYTh] = []
        prev = at
        for p in poses:
            turn = abs(math.degrees(p[2] - prev[2])) / SWEEP_STEP_DEG
            n = max(1, math.ceil(max(turn, math.dist(p[:2], prev[:2]) / SWEEP_STEP_M)))
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

    def allowed(self, cost: CostMap, at: XYTh, poses: list[XYTh]) -> bool:
        """Each outline point, along the whole move: never nearer than keep_out_m to a known obstacle, or
        than it is now when it already is (the points move with the body, so each is its own)."""
        floor = np.minimum(cost.settings.keep_out_m, cost.dist_many(self.world(at))) - 1e-9
        return all(bool((cost.dist_many(self.world(p)) >= floor).all()) for p in self.dense(at, poses))

    def highest_cost(self, cost: CostMap, at: XYTh, poses: list[XYTh], ahead_m: float | None = None) -> float:
        """The dearest cell the outline meets along the move (only within ahead_m of `at` when given)."""
        best = 0.0
        for p in self.dense(at, poses):
            if ahead_m is not None and math.dist(p[:2], at[:2]) > ahead_m + 1e-9:
                break
            best = max(best, float(cost.outline_cost(self.world(p)).max()))
        return best

    @staticmethod
    def spin_poses(at: XYTh, left: bool, deg: float = 90.0) -> list[XYTh]:
        """A turn on the spot, SWEEP_STEP_DEG at a time, the way it turns."""
        n = max(1, math.ceil(deg / SWEEP_STEP_DEG))
        sign = 1.0 if left else -1.0
        return [(at[0], at[1], at[2] + sign * math.radians(deg * k / n)) for k in range(1, n + 1)]

    @staticmethod
    def back_poses(at: XYTh, length_m: float) -> list[XYTh]:
        """Straight back, SWEEP_STEP_M at a time."""
        n = max(1, math.ceil(length_m / SWEEP_STEP_M - 1e-9))
        c, s = math.cos(at[2]), math.sin(at[2])
        return [(at[0] - length_m * k / n * c, at[1] - length_m * k / n * s, at[2]) for k in range(1, n + 1)]
