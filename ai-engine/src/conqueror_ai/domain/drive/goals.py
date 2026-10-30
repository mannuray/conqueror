"""Where to go (AI engine LLD §18.7, §18.13.12, spec 9 §4.1): wander picks seen, open, reachable floor a
little ahead, preferring places it hasn't been lately (so it drifts to the edge of what it has seen, and on),
now and then a new spot; explore picks the cheapest edge between seen floor and unseen space until none is
left."""

from __future__ import annotations

import math
import random
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.planner import bearing_deg
from conqueror_ai.domain.drive.route import GRID_M, XY, RoutePlanner

REACHED_M = 0.4  # a destination this near counts as reached

WANDER_RADII_M = (1.5, 1.75, 2.0)
WANDER_STEP_DEG = 10
WANDER_OPEN_M = 0.30  # a destination this far from any known obstacle, on seen floor
WANDER_NEW_S = 6.0
WANDER_NOISE = 0.3  # the score's random part (a standard deviation)
VISIT_CELL_M = 0.25  # where the tank has been, remembered in cells this big ...
VISIT_NEAR_M = 0.5  # ... a spot this near a visit counts as visited ...
VISIT_FORGET_S = 60.0  # ... and forgotten over this long
NOVEL_W = 2.0  # an unvisited spot behind (-1 + 2) beats a visited one ahead (+1)

EXPLORE_NEW_S = 3.0
UNSEEN_MIN_CELLS = 9  # unseen bits smaller than 30 x 30 cm are ignored (spec 9 §4.1)
FRONTIER_MIN_CELLS = 3  # a stretch of frontier shorter than 30 cm is ignored
VISITED_M = 0.30  # reached frontiers are remembered in cells this big


class WanderGoal:
    def __init__(self, seconds: float | None, rng: random.Random) -> None:
        self.seconds = seconds
        self.rng = rng
        self._t0: float | None = None
        self._dest: XY | None = None
        self._picked_s = 0.0
        self._visits: dict[tuple[int, int], float] = {}  # visit cell -> when the tank was last in it

    def drop(self) -> None:
        """The current destination has no route: pick another next time."""
        self._dest = None

    def done(self, now_s: float) -> bool:
        return self.seconds is not None and self._t0 is not None and now_s - self._t0 >= self.seconds - 1e-9

    def destination(self, now_s: float, at: XYTh, cost: CostMap, planner: RoutePlanner) -> XY | None:
        if self._t0 is None:
            self._t0 = now_s
        self._visits[self._visit_key(at[:2])] = now_s
        d = self._dest
        if d is not None and math.dist(d, at[:2]) > REACHED_M and now_s - self._picked_s < WANDER_NEW_S:
            return d
        reach = planner.costs(cost, (at[0], at[1]))  # spots behind a wall are skipped, not tried and dropped
        best: tuple[float, XY] | None = None
        for r in WANDER_RADII_M:
            for k in range(0, 360, WANDER_STEP_DEG):
                a = at[2] + math.radians(k)
                p = (at[0] + r * math.cos(a), at[1] + r * math.sin(a))
                room = cost.dist_m(*p)
                if room < WANDER_OPEN_M or not cost.seen_at(*p) or reach[planner.grid_of(*p)] == math.inf:
                    continue
                score = math.cos(math.radians(bearing_deg(at, p))) + 0.5 * min(room, 1.0)
                score += NOVEL_W * self._novelty(p, now_s) + self.rng.gauss(0.0, WANDER_NOISE)
                if best is None or score > best[0]:
                    best = (score, p)
        self._dest, self._picked_s = (None if best is None else best[1]), now_s
        return self._dest

    @staticmethod
    def _visit_key(p: XY) -> tuple[int, int]:
        return math.floor(p[0] / VISIT_CELL_M), math.floor(p[1] / VISIT_CELL_M)

    def _novelty(self, p: XY, now_s: float) -> float:
        """1 where the tank has never been near; after a visit, rising back to 1 over VISIT_FORGET_S."""
        i0, j0 = self._visit_key(p)
        n = math.ceil(VISIT_NEAR_M / VISIT_CELL_M)
        last = max(
            (
                t
                for i in range(i0 - n, i0 + n + 1)
                for j in range(j0 - n, j0 + n + 1)
                if (t := self._visits.get((i, j))) is not None
                and math.dist(p, ((i + 0.5) * VISIT_CELL_M, (j + 0.5) * VISIT_CELL_M)) <= VISIT_NEAR_M
            ),
            default=None,
        )
        return 1.0 if last is None else min(1.0, (now_s - last) / VISIT_FORGET_S)


class ExploreGoal:
    def __init__(self) -> None:
        self.done = False
        self._dest: XY | None = None
        self._picked_s = 0.0
        self._visited: set[tuple[int, int]] = set()

    def drop(self) -> None:
        """The current target has no route (the map changed since it was picked): pick again next time."""
        self._dest = None

    @staticmethod
    def _key(p: XY) -> tuple[int, int]:
        return math.floor(p[0] / VISITED_M), math.floor(p[1] / VISITED_M)

    def destination(self, now_s: float, at: XYTh, cost: CostMap, planner: RoutePlanner) -> XY | None:
        """The cheapest reachable frontier; None (and done) when none is left."""
        d = self._dest
        if d is not None and math.dist(d, at[:2]) <= REACHED_M:
            self._visited.add(self._key(d))
            d = None
        if d is not None and now_s - self._picked_s < EXPLORE_NEW_S:
            return d
        frontier = self._frontier(cost, planner.half_w)

        def accept(i: int, j: int) -> bool:  # an unvisited frontier cell; reached ones are remembered
            if not frontier[i, j]:
                return False
            p = planner.grid_centre(i, j)
            if math.dist(p, at[:2]) <= REACHED_M:
                self._visited.add(self._key(p))
                return False
            return self._key(p) not in self._visited

        # the cheapest frontier is the first an outward search reaches (review M1: no whole-map search)
        ij = planner.nearest(cost, (at[0], at[1]), accept)
        best = None if ij is None else (0.0, planner.grid_centre(*ij))
        self._dest, self._picked_s = (None if best is None else best[1]), now_s
        self.done = best is None
        return self._dest

    @staticmethod
    def _frontier(cost: CostMap, half_w: float) -> NDArray[np.bool_]:
        """Seen, open 10 cm cells next to unseen space (bits under 30 x 30 cm ignored); stretches of frontier
        under 30 cm dropped."""
        k = max(1, round(GRID_M / cost.cell_m))
        g = cost.n // k
        seen = cast("NDArray[np.bool_]", cost.seen[: g * k, : g * k].reshape(g, k, g, k).any(axis=(1, 3)))
        dist = cast("NDArray[np.float64]", cost.dist[: g * k, : g * k].reshape(g, k, g, k).min(axis=(1, 3)))
        unseen_big = _big_parts(~seen, UNSEEN_MIN_CELLS).astype(np.uint8)
        near_unseen = cast("NDArray[np.uint8]", cv2.dilate(unseen_big, np.ones((3, 3), np.uint8))) > 0
        return _big_parts(seen & (dist >= half_w) & near_unseen, FRONTIER_MIN_CELLS)


def _big_parts(mask: NDArray[np.bool_], min_cells: int) -> NDArray[np.bool_]:
    """The 8-connected parts of a mask with at least min_cells cells."""
    out = cv2.connectedComponentsWithStats(mask.astype(np.uint8), connectivity=8)
    n, labels, stats = cast("tuple[int, NDArray[np.int32], NDArray[np.int32]]", out[:3])
    keep = np.zeros(n, np.bool_)
    keep[1:] = stats[1:, cv2.CC_STAT_AREA] >= min_cells
    return keep[labels]
