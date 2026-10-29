"""Where to go (AI engine LLD §18.7, spec 9 §4.1): wander picks open floor a little ahead, now and then a new
spot; explore picks the cheapest edge between seen floor and unseen space until none is left."""

from __future__ import annotations

import math
import random
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.planner import bearing_deg
from conqueror_ai.domain.drive.route import GRID_M, ROBOT_R_M, XY, RoutePlanner

REACHED_M = 0.4  # a destination this near counts as reached

WANDER_RADII_M = (1.5, 1.75, 2.0)
WANDER_STEP_DEG = 10
WANDER_OPEN_M = 0.30  # a destination needs this much clearance, on seen floor
WANDER_NEW_S = 6.0
WANDER_NOISE = 0.3  # the score's random part (a standard deviation)

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

    def drop(self) -> None:
        """The current destination has no route: pick another next time."""
        self._dest = None

    def done(self, now_s: float) -> bool:
        return self.seconds is not None and self._t0 is not None and now_s - self._t0 >= self.seconds - 1e-9

    def destination(self, now_s: float, at: XYTh, clear: ClearanceMap) -> XY | None:
        if self._t0 is None:
            self._t0 = now_s
        d = self._dest
        if d is not None and math.dist(d, at[:2]) > REACHED_M and now_s - self._picked_s < WANDER_NEW_S:
            return d
        best: tuple[float, XY] | None = None
        for r in WANDER_RADII_M:
            for k in range(0, 360, WANDER_STEP_DEG):
                a = at[2] + math.radians(k)
                p = (at[0] + r * math.cos(a), at[1] + r * math.sin(a))
                room = clear.dist_m(*p)
                if room < WANDER_OPEN_M or not clear.seen_at(*p):
                    continue
                score = math.cos(math.radians(bearing_deg(at, p))) + 0.5 * min(room, 1.0)
                score += self.rng.gauss(0.0, WANDER_NOISE)
                if best is None or score > best[0]:
                    best = (score, p)
        self._dest, self._picked_s = (None if best is None else best[1]), now_s
        return self._dest


class ExploreGoal:
    def __init__(self) -> None:
        self.done = False
        self._dest: XY | None = None
        self._picked_s = 0.0
        self._visited: set[tuple[int, int]] = set()

    @staticmethod
    def _key(p: XY) -> tuple[int, int]:
        return math.floor(p[0] / VISITED_M), math.floor(p[1] / VISITED_M)

    def destination(self, now_s: float, at: XYTh, clear: ClearanceMap, planner: RoutePlanner) -> XY | None:
        """The cheapest reachable frontier; None (and done) when none is left."""
        d = self._dest
        if d is not None and math.dist(d, at[:2]) <= REACHED_M:
            self._visited.add(self._key(d))
            d = None
        if d is not None and now_s - self._picked_s < EXPLORE_NEW_S:
            return d
        costs = planner.costs(clear, (at[0], at[1]))
        frontier = self._frontier(clear)
        cand = frontier & np.isfinite(costs)
        best: tuple[float, XY] | None = None
        for i, j in zip(*np.nonzero(cand), strict=True):
            p = planner.grid_centre(int(i), int(j))
            if self._key(p) in self._visited or math.dist(p, at[:2]) <= REACHED_M:
                if math.dist(p, at[:2]) <= REACHED_M:
                    self._visited.add(self._key(p))
                continue
            if best is None or costs[i, j] < best[0]:
                best = (float(costs[i, j]), p)
        self._dest, self._picked_s = (None if best is None else best[1]), now_s
        self.done = best is None
        return self._dest

    @staticmethod
    def _frontier(clear: ClearanceMap) -> NDArray[np.bool_]:
        """Seen, open 10 cm cells next to unseen space (bits under 30 x 30 cm ignored); stretches of frontier
        under 30 cm dropped."""
        k = max(1, round(GRID_M / clear.cell_m))
        g = clear.n // k
        seen = cast("NDArray[np.bool_]", clear.seen[: g * k, : g * k].reshape(g, k, g, k).any(axis=(1, 3)))
        dist = cast("NDArray[np.float64]", clear.dist[: g * k, : g * k].reshape(g, k, g, k).min(axis=(1, 3)))
        unseen_big = _big_parts(~seen, UNSEEN_MIN_CELLS).astype(np.uint8)
        near_unseen = cast("NDArray[np.uint8]", cv2.dilate(unseen_big, np.ones((3, 3), np.uint8))) > 0
        return _big_parts(seen & (dist >= ROBOT_R_M) & near_unseen, FRONTIER_MIN_CELLS)


def _big_parts(mask: NDArray[np.bool_], min_cells: int) -> NDArray[np.bool_]:
    """The 8-connected parts of a mask with at least min_cells cells."""
    out = cv2.connectedComponentsWithStats(mask.astype(np.uint8), connectivity=8)
    n, labels, stats = cast("tuple[int, NDArray[np.int32], NDArray[np.int32]]", out[:3])
    keep = np.zeros(n, np.bool_)
    keep[1:] = stats[1:, cv2.CC_STAT_AREA] >= min_cells
    return keep[labels]
