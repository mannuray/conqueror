"""Routes on the room map (AI engine LLD §18.6, spec 9 §4.2): A* on a 10 cm grid (each cell the worst of its
2 x 2 map cells), 8-connected, around everything closer than the tank's half-width + a margin. Cells near
obstacles and unseen cells cost more; the start cell is always allowed (the tank may stand near a wall)."""

from __future__ import annotations

import heapq
import itertools
import math

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.clearance import ClearanceMap

XY = tuple[float, float]

ROBOT_R_M = 0.15  # half the tank's width + a margin: the route keeps its centre this far from obstacles
NEAR_W = 3.0  # extra cost close to obstacles (inside NEAR_FROM_M), up to this much per metre ...
NEAR_FROM_M = 0.35
NEAR_SPAN_M = 0.20
UNSEEN_W = 1.0  # ... and this much more through unseen cells
GRID_M = 0.10
ESCAPE_M = 0.30  # near the start, cells no closer to an obstacle than the start are allowed (the way out)
SQRT2 = math.sqrt(2.0)
STEPS = (
    (1, 0, 1.0),
    (-1, 0, 1.0),
    (0, 1, 1.0),
    (0, -1, 1.0),
    (1, 1, SQRT2),
    (1, -1, SQRT2),
    (-1, 1, SQRT2),
    (-1, -1, SQRT2),
)


class _Grid:
    """The 10 cm grid of one clearance map: per cell the cost factor per metre, inf where not allowed."""

    def __init__(self, clear: ClearanceMap) -> None:
        k = max(1, round(GRID_M / clear.cell_m))
        self.k, self.g = k, clear.n // k
        self.cell_m = clear.cell_m * k
        self.half_m = clear.half_m
        g = self.g
        d = clear.dist[: g * k, : g * k].reshape(g, k, g, k).min(axis=(1, 3))
        seen = clear.seen[: g * k, : g * k].reshape(g, k, g, k).any(axis=(1, 3))
        self.d: FloatArray = d
        self.near: FloatArray = (
            1.0 + NEAR_W * np.clip(NEAR_FROM_M - d, 0.0, None) / NEAR_SPAN_M + UNSEEN_W * ~seen
        )
        self.factor: FloatArray = np.where(d >= ROBOT_R_M, self.near, np.inf)

    def factors_from(self, s: tuple[int, int]) -> list[float]:
        """The cost factors for a search from cell s: the start allowed, and near it (ESCAPE_M) every cell no
        closer to an obstacle than the start, so a tank standing close to a wall can always move away."""
        f = self.factor.copy()
        r = math.ceil(ESCAPE_M / self.cell_m)
        i0, i1, j0, j1 = (
            max(0, s[0] - r),
            min(self.g, s[0] + r + 1),
            max(0, s[1] - r),
            min(self.g, s[1] + r + 1),
        )
        box = (slice(i0, i1), slice(j0, j1))
        away = (self.d[box] >= self.d[s] - 1e-9) & (self.d[box] > 0)
        f[box] = np.where(away & np.isinf(f[box]), self.near[box], f[box])
        f[s] = min(float(f[s]), float(self.near[s]))
        return f.ravel().tolist()

    def of(self, x: float, y: float) -> tuple[int, int] | None:
        i, j = math.floor((x + self.half_m) / self.cell_m), math.floor((y + self.half_m) / self.cell_m)
        return (i, j) if 0 <= i < self.g and 0 <= j < self.g else None

    def centre(self, i: int, j: int) -> XY:
        return ((i + 0.5) * self.cell_m - self.half_m, (j + 0.5) * self.cell_m - self.half_m)


class RoutePlanner:
    def __init__(self) -> None:
        self._grid: _Grid | None = None
        self._clear: ClearanceMap | None = None

    def _grid_for(self, clear: ClearanceMap) -> _Grid:
        if self._clear is not clear or self._grid is None:  # one grid per snapshot's clearance
            self._grid, self._clear = _Grid(clear), clear
        return self._grid

    def grid_of(self, x: float, y: float) -> tuple[int, int]:
        """The 10 cm grid cell of a room point (for reading costs()); the last grid built."""
        if self._grid is None:
            raise ValueError("no grid yet: call route() or costs() first")
        ij = self._grid.of(x, y)
        if ij is None:
            raise ValueError(f"({x}, {y}) is outside the map")
        return ij

    def grid_centre(self, i: int, j: int) -> XY:
        if self._grid is None:
            raise ValueError("no grid yet: call route() or costs() first")
        return self._grid.centre(i, j)

    def route(
        self, clear: ClearanceMap, start: XY, goal: XY, goal_ok: bool = False, near_m: float = 0.0
    ) -> list[XY] | None:
        """Room points from start to goal (both exact at the ends), or None: no way, or an end off the map.
        goal_ok: the goal's own cell is allowed whatever its clearance (the chase's ball). near_m: the way may
        end at any allowed cell this near the goal (the chase stops that far off anyway; review I2: a ball
        against a sofa has no open cell beside it)."""
        grid = self._grid_for(clear)
        s, t = grid.of(*start), grid.of(*goal)
        if s is None or t is None:
            return None
        g, step = grid.g, grid.cell_m
        factor = grid.factors_from(s)
        si, ti = s[0] * g + s[1], t[0] * g + t[1]
        if not goal_ok and math.isinf(factor[ti]):
            return None
        factor[ti] = factor[ti] if math.isfinite(factor[ti]) else 1.0
        tx, ty = t
        reach = near_m / step  # in cells

        def h(i: int, j: int) -> float:  # never more than the real cost (factor >= 1)
            dx, dy = abs(i - tx), abs(j - ty)
            if reach > 0:  # straight-line distance to the reach circle
                return step * max(0.0, math.hypot(dx, dy) - reach)
            return step * (max(dx, dy) + (SQRT2 - 1.0) * min(dx, dy))

        def arrived(i: int, j: int) -> bool:
            return (i * g + j) == ti or (reach > 0 and math.hypot(i - tx, j - ty) <= reach)

        best = {si: 0.0}
        came: dict[int, int] = {}
        heap = [(h(*s), 0.0, si)]
        end: int | None = None
        while heap:
            _, cost, cur = heapq.heappop(heap)
            if arrived(*divmod(cur, g)):
                end = cur
                break
            if cost > best.get(cur, math.inf):
                continue
            ci, cj = divmod(cur, g)
            for di, dj, ln in STEPS:
                ni, nj = ci + di, cj + dj
                if not (0 <= ni < g and 0 <= nj < g):
                    continue
                nxt = ni * g + nj
                f = factor[nxt]
                if f == math.inf:
                    continue
                c = cost + ln * step * f
                if c < best.get(nxt, math.inf):
                    best[nxt] = c
                    came[nxt] = cur
                    heapq.heappush(heap, (c + h(ni, nj), c, nxt))
        if end is None:
            return None
        cells = [end]
        while cells[-1] != si:
            cells.append(came[cells[-1]])
        cells.reverse()
        mid = [grid.centre(*divmod(c, g)) for c in cells[1:-1]]
        if end != ti:  # ended within reach: its cell, then the goal itself (to steer at)
            mid.append(grid.centre(*divmod(end, g)))
        return [start, *mid, goal]

    def costs(self, clear: ClearanceMap, start: XY) -> FloatArray:
        """The route cost from start to every 10 cm grid cell (Dijkstra), inf where unreachable."""
        grid = self._grid_for(clear)
        g, step = grid.g, grid.cell_m
        out = np.full((g, g), np.inf)
        s = grid.of(*start)
        if s is None:
            return out
        factor = grid.factors_from(s)
        si = s[0] * g + s[1]
        best = {si: 0.0}
        heap = [(0.0, si)]
        while heap:
            cost, cur = heapq.heappop(heap)
            if cost > best.get(cur, math.inf):
                continue
            ci, cj = divmod(cur, g)
            for di, dj, ln in STEPS:
                ni, nj = ci + di, cj + dj
                if 0 <= ni < g and 0 <= nj < g:
                    nxt = ni * g + nj
                    f = factor[nxt]
                    if f != math.inf:
                        c = cost + ln * step * f
                        if c < best.get(nxt, math.inf):
                            best[nxt] = c
                            heapq.heappush(heap, (c, nxt))
        flat = out.ravel()
        for k, v in best.items():
            flat[k] = v
        return out


def aim_point(route: list[XY], at: XY, ahead_m: float = 0.5) -> XY:
    """The point ahead_m along the route from the route point nearest the tank (the last when shorter)."""
    if len(route) == 1:
        return route[0]
    near = min(range(len(route)), key=lambda i: math.dist(route[i], at))
    left = ahead_m
    for a, b in itertools.pairwise(route[near:]):
        seg = math.dist(a, b)
        if seg >= left > 0:
            f = left / seg
            return (a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1]))
        left -= seg
    return route[-1]
