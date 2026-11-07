"""Today's route planner (AI engine LLD §18.13.3), kept as a test-only reference for Plan Z12 (LLD §27.4): the
quick planner must give exactly its routes, costs and nearest cells."""

from __future__ import annotations

import heapq
import math
from collections.abc import Callable

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.route import (
    SQRT2,
    STEPS,
    XY,
    _Grid,  # pyright: ignore[reportPrivateUsage]
)


def _factors_from(grid: _Grid, s: tuple[int, int]) -> list[float]:
    f = grid.factor.copy()
    if math.isinf(f[s]):
        f[s] = 1.0
    return f.ravel().tolist()


class ReferencePlanner:
    """Today's planner, verbatim (Plan Z12 Task 1): what the quick one must give exactly."""

    def __init__(self, lim: Limits | None = None) -> None:
        """lim: the tank's body (its half-width closes cells)."""
        self.half_w = (lim or Limits()).body_half_w_m
        self._grid: _Grid | None = None
        self._cost: CostMap | None = None

    def _grid_for(self, cost: CostMap) -> _Grid:
        if self._cost is not cost or self._grid is None:  # one grid per snapshot's cost map
            self._grid, self._cost = _Grid(cost, self.half_w), cost
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
        self, cost: CostMap, start: XY, goal: XY, goal_ok: bool = False, near_m: float = 0.0
    ) -> list[XY] | None:
        """Room points from start to goal (both exact at the ends), or None: no way, or an end off the map.
        goal_ok: the goal's own cell is allowed whatever its cost (the chase's ball). near_m: the way may
        end at any allowed cell this near the goal (the chase stops that far off anyway; review I2: a ball
        against a sofa has no open cell beside it)."""
        grid = self._grid_for(cost)
        s, t = grid.of(*start), grid.of(*goal)
        if s is None or t is None:
            return None
        g, step = grid.g, grid.cell_m
        factor = _factors_from(grid, s)
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
            _, so_far, cur = heapq.heappop(heap)
            if arrived(*divmod(cur, g)):
                end = cur
                break
            if so_far > best.get(cur, math.inf):
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
                c = so_far + ln * step * f
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

    def nearest(self, cost: CostMap, start: XY, accept: Callable[[int, int], bool]) -> tuple[int, int] | None:
        """The cheapest-to-reach 10 cm grid cell that `accept` takes (Dijkstra, stopping at the first one -
        no whole-map search), or None when none is reachable."""
        grid = self._grid_for(cost)
        g, step = grid.g, grid.cell_m
        s = grid.of(*start)
        if s is None:
            return None
        factor = _factors_from(grid, s)
        si = s[0] * g + s[1]
        best = {si: 0.0}
        heap = [(0.0, si)]
        while heap:
            so_far, cur = heapq.heappop(heap)
            if so_far > best.get(cur, math.inf):
                continue
            ci, cj = divmod(cur, g)
            if accept(ci, cj):
                return ci, cj
            for di, dj, ln in STEPS:
                ni, nj = ci + di, cj + dj
                if 0 <= ni < g and 0 <= nj < g:
                    nxt = ni * g + nj
                    f = factor[nxt]
                    if f != math.inf:
                        c = so_far + ln * step * f
                        if c < best.get(nxt, math.inf):
                            best[nxt] = c
                            heapq.heappush(heap, (c, nxt))
        return None

    def costs(self, cost: CostMap, start: XY) -> FloatArray:
        """The route cost from start to every 10 cm grid cell (Dijkstra), inf where unreachable."""
        grid = self._grid_for(cost)
        g, step = grid.g, grid.cell_m
        out = np.full((g, g), np.inf)
        s = grid.of(*start)
        if s is None:
            return out
        factor = _factors_from(grid, s)
        si = s[0] * g + s[1]
        best = {si: 0.0}
        heap = [(0.0, si)]
        while heap:
            so_far, cur = heapq.heappop(heap)
            if so_far > best.get(cur, math.inf):
                continue
            ci, cj = divmod(cur, g)
            for di, dj, ln in STEPS:
                ni, nj = ci + di, cj + dj
                if 0 <= ni < g and 0 <= nj < g:
                    nxt = ni * g + nj
                    f = factor[nxt]
                    if f != math.inf:
                        c = so_far + ln * step * f
                        if c < best.get(nxt, math.inf):
                            best[nxt] = c
                            heapq.heappush(heap, (c, nxt))
        flat = out.ravel()
        for k, v in best.items():
            flat[k] = v
        return out
