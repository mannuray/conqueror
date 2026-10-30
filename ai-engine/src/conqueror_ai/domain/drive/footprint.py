"""One body rule (AI engine LLD §18.13.2, Plan Z6): the outline of the tank's rectangle (Limits, design-notes
§163), rolled out along a move. No outline point may come nearer than the keep-out to a known obstacle, and a
point already nearer may not come any nearer; everything else about nearness is cost (§18.13.1)."""

from __future__ import annotations

import math

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.limits import Limits

OUTLINE_STEP_M = 0.025  # outline points this far apart (the map's cells are 5 cm)
SWEEP_STEP_DEG = 5.0  # a move is checked at poses at most this far turned ...
SWEEP_STEP_M = 0.025  # ... and this far moved apart
GAP_TOL_M = 0.005  # already inside the keep-out: nearer than now by less than this doesn't count (§171)
SLIDE_MIN_M = 0.03  # nearer than this already: only a move that ends further off (owner, design-notes §171)


class Footprint:
    def __init__(self, lim: Limits) -> None:
        f, r, w = lim.body_front_m, lim.body_rear_m, lim.body_half_w_m
        self.front_m, self.rear_m, self.half_w_m = f, r, w
        self.lim_reach = math.hypot(max(f, r), w)
        ys = np.linspace(-w, w, max(2, math.ceil(2 * w / OUTLINE_STEP_M) + 1))
        xs = np.linspace(-r, f, max(2, math.ceil((f + r) / OUTLINE_STEP_M) + 1))[1:-1]
        edges = [np.stack([np.full_like(ys, f), ys], 1), np.stack([np.full_like(ys, -r), ys], 1)]
        sides = [np.stack([xs, np.full_like(xs, w)], 1), np.stack([xs, np.full_like(xs, -w)], 1)]
        # the outline (tank frame: x ahead, y left) reads the cost; the whole body keeps the keep-out (review
        # I1: a point already in an obstacle's cell can't get nearer, so with the outline alone a thing under
        # the edge could end up inside the body)
        self.outline: FloatArray = np.concatenate(edges + sides).astype(np.float64)
        gx, gy = np.meshgrid(np.linspace(-r, f, len(xs) + 2), ys)
        self.body: FloatArray = np.stack([gx.ravel(), gy.ravel()], axis=1).astype(np.float64)

    def world(self, p: XYTh, pts: FloatArray | None = None) -> FloatArray:
        """These points of the tank (the outline by default) in the room at pose p."""
        c, s = math.cos(p[2]), math.sin(p[2])
        o = self.outline if pts is None else pts
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
        """Along the whole move: no point of the body nearer than keep_out_m to a known obstacle, or - when
        the body already is - than its closest point is now (GAP_TOL_M aside); and no obstacle comes inside
        the body's outline (review I1: a leg under the front edge). Nearer than SLIDE_MIN_M already, the move
        must end further off (no sliding along a thing that overlaps the track). The body's gap, measured
        exactly, not each point's own from cell centres: that read sliding past one mark as nearer for some
        point and froze every move (floor run 3, design-notes §171)."""
        d = float(cost.gap_many(self.world(at, self.body)).min())
        floor = min(cost.settings.keep_out_m, d - GAP_TOL_M) - 1e-9
        dense = self.dense(at, poses)
        if not dense:
            return True
        reach = self.lim_reach
        xs = [p[0] for p in (at, *dense)]
        ys = [p[1] for p in (at, *dense)]
        marks = cost.marks_within(min(xs) - reach, min(ys) - reach, max(xs) + reach, max(ys) + reach)
        was_in = self._inside(at, marks)
        if d < SLIDE_MIN_M and float(cost.gap_many(self.world(dense[-1], self.body)).min()) < d + GAP_TOL_M:
            return False
        return all(
            bool((cost.gap_many(self.world(p, self.body)) >= floor).all())
            and not bool((self._inside(p, marks) & ~was_in).any())
            for p in dense
        )

    def _inside(self, p: XYTh, xy: FloatArray) -> NDArray[np.bool_]:
        """Which of these room points lie inside the body's outline at pose p."""
        c, s = math.cos(p[2]), math.sin(p[2])
        dx, dy = xy[:, 0] - p[0], xy[:, 1] - p[1]
        bx, by = dx * c + dy * s, -dx * s + dy * c
        return (bx > -self.rear_m) & (bx < self.front_m) & (np.abs(by) < self.half_w_m)

    def highest_cost(self, cost: CostMap, at: XYTh, poses: list[XYTh], ahead_m: float | None = None) -> float:
        """The dearest cell the outline meets along the move (only within ahead_m of `at` when given)."""
        best = 0.0
        for p in self.dense(at, poses):
            if ahead_m is not None and math.dist(p[:2], at[:2]) > ahead_m + 1e-9:
                break
            best = max(best, float(cost.outline_cost(self.world(p)).max()))
        return best

    def mean_cost(self, cost: CostMap, at: XYTh, poses: list[XYTh]) -> float:
        """The mean over the move of the dearest cell the outline meets at each pose (the fan's score: the
        highest alone is met at the start for every move beside a wall, so it can't tell moving away)."""
        dense = self.dense(at, poses)
        return sum(float(cost.outline_cost(self.world(p)).max()) for p in dense) / max(1, len(dense))

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
