"""One body rule (AI engine LLD §18.13.14, Plan Z6): the tank's rectangle (Limits, design-notes §163),
rolled out along a move, never comes nearer to a known obstacle than the keep-out - or, already nearer, than
it is now. Gaps are exact, from the rectangle to the obstacles' cell centres (§171); everything else about
nearness is cost (§18.13.1)."""

from __future__ import annotations

import math
from typing import cast

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.limits import Limits

OUTLINE_STEP_M = 0.025  # outline points this far apart (the map's cells are 5 cm)
SWEEP_STEP_DEG = 5.0  # a move is checked at poses at most this far turned ...
SWEEP_STEP_M = 0.025  # ... and this far moved apart
SLIDE_MIN_M = 0.03  # nearer than this already: only a move that ends further off (owner, design-notes §171)


class Footprint:
    def __init__(self, lim: Limits) -> None:
        f, r, w = lim.body_front_m, lim.body_rear_m, lim.body_half_w_m
        self.front_m, self.rear_m, self.half_w_m = f, r, w
        self.lim_reach = lim.sweep_r_m
        ys = np.linspace(-w, w, max(2, math.ceil(2 * w / OUTLINE_STEP_M) + 1))
        xs = np.linspace(-r, f, max(2, math.ceil((f + r) / OUTLINE_STEP_M) + 1))[1:-1]
        edges = [np.stack([np.full_like(ys, f), ys], 1), np.stack([np.full_like(ys, -r), ys], 1)]
        sides = [np.stack([xs, np.full_like(xs, w)], 1), np.stack([xs, np.full_like(xs, -w)], 1)]
        # the outline (tank frame: x ahead, y left) reads the cost; the whole body's points find the cells it
        # covers (backing off a mark under it, recovery)
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
        """Along the whole move the body's gap never drops below min(keep_out_m, the gap now); nearer than
        SLIDE_MIN_M already, the move must also end further off (no sliding along a thing that overlaps the
        track, owner). A mark inside the outline is a gap below 0, deeper is nearer (review I1: a leg under
        the front edge can't end up inside the body)."""
        dense = self.dense(at, poses)
        if not dense:
            return True
        keep = cost.settings.keep_out_m
        out = self.lim_reach + keep  # marks further from every pose than this can't matter
        xs = [p[0] for p in (at, *dense)]
        ys = [p[1] for p in (at, *dense)]
        marks = cost.marks_within(min(xs) - out, min(ys) - out, max(xs) + out, max(ys) + out)
        g = self.gaps([at, *dense], marks)
        if bool((g[1:] < min(keep, float(g[0])) - 1e-9).any()):
            return False
        return not (g[0] < SLIDE_MIN_M and g[-1] <= g[0] + 1e-9)

    def gaps(self, poses: list[XYTh], marks: FloatArray) -> FloatArray:
        """The body's gap at each pose: the distance from its rectangle to the nearest of these points, or,
        for a point inside it, minus that point's depth (deeper is nearer); inf with no points (§171)."""
        if marks.shape[0] == 0:
            return np.full(len(poses), np.inf)
        p = np.asarray(poses, dtype=np.float64)
        c, s = np.cos(p[:, 2])[:, None], np.sin(p[:, 2])[:, None]
        dx, dy = marks[None, :, 0] - p[:, 0, None], marks[None, :, 1] - p[:, 1, None]
        bx, by = dx * c + dy * s, np.abs(-dx * s + dy * c)
        f, r, w = self.front_m, self.rear_m, self.half_w_m
        outside = np.hypot(np.maximum(np.maximum(-r - bx, bx - f), 0.0), np.maximum(by - w, 0.0))
        depth = np.minimum(np.minimum(bx + r, f - bx), w - by)
        return cast("FloatArray", np.where(depth > 0.0, -depth, outside).min(axis=1))

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
