"""Routes on the cost map (AI engine LLD §18.13.3, Plan Z6): A* on a 10 cm grid (each cell the worst of its
2 x 2 map cells: the nearest obstacle, the dearest cost), 8-connected. A cell is closed only where the tank's
centre is nearer than its half-width to a known obstacle (the body would overlap it going straight); every
step costs its length x (1 + route_cost_w x the cell's centre cost), so a route keeps room to turn round, and
off unknown floor, where it can - no rules (design-notes §166). The start cell is always open."""

from __future__ import annotations

import heapq
import itertools
import math
from collections.abc import Callable

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.limits import Limits

XY = tuple[float, float]

GRID_M = 0.10
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
    """The 10 cm grid of one cost map: per cell the step factor per metre, inf where closed."""

    def __init__(self, cost: CostMap, half_w: float) -> None:
        k = max(1, round(GRID_M / cost.cell_m))
        self.k, self.g = k, cost.n // k
        self.cell_m = cost.cell_m * k
        self.half_m = cost.half_m
        g = self.g
        d = cost.dist[: g * k, : g * k].reshape(g, k, g, k).min(axis=(1, 3))
        cc = cost.centre_cost(half_w)[: g * k, : g * k].reshape(g, k, g, k).max(axis=(1, 3))
        self.factor: FloatArray = np.where(d >= half_w, 1.0 + cost.settings.route_cost_w * cc, np.inf)
        # The searches run on flat lists over the grid padded with closed cells (LLD §27.2): no bounds check,
        # the cell numbers in today's order (row, then column), the eight steps as offsets in STEPS' order.
        # The searches run on flat lists over the grid padded with closed cells (LLD §27.2): no bounds check,
        # the cell numbers in today's order (row, then column), the eight steps as offsets in STEPS' order.
        self.w = g + 2
        self.padded: list[float] = np.pad(self.factor, 1, constant_values=np.inf).ravel().tolist()
        self.steps: list[tuple[int, float]] = [(di * self.w + dj, ln * self.cell_m) for di, dj, ln in STEPS]

    def pad(self, i: int, j: int) -> int:
        return (i + 1) * self.w + j + 1

    def unpad(self, k: int) -> tuple[int, int]:
        i, j = divmod(k, self.w)
        return i - 1, j - 1

    def octile_from(self, t: tuple[int, int], k2: float) -> list[float]:
        """Per padded cell the octile distance to cell t in metres (A*'s h without a reach, LLD §27.2)."""
        idx = np.arange(-1, self.g + 1)
        dx = np.abs(idx - t[0])[:, None]
        dy = np.abs(idx - t[1])[None, :]
        h = self.cell_m * (np.maximum(dx, dy) + k2 * np.minimum(dx, dy))
        return h.ravel().tolist()

    def search_from(self, s: tuple[int, int]) -> list[float]:
        """The padded step factors for a search from cell s: the start cell open (the tank is there)."""
        f = self.padded.copy()
        k = self.pad(*s)
        if math.isinf(f[k]):
            f[k] = 1.0
        return f

    def of(self, x: float, y: float) -> tuple[int, int] | None:
        i, j = math.floor((x + self.half_m) / self.cell_m), math.floor((y + self.half_m) / self.cell_m)
        return (i, j) if 0 <= i < self.g and 0 <= j < self.g else None

    def centre(self, i: int, j: int) -> XY:
        return ((i + 0.5) * self.cell_m - self.half_m, (j + 0.5) * self.cell_m - self.half_m)


class RoutePlanner:
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
        step, steps, w = grid.cell_m, grid.steps, grid.w
        factor = grid.search_from(s)
        si, ti = grid.pad(*s), grid.pad(*t)
        if not goal_ok and math.isinf(factor[ti]):
            return None
        factor[ti] = factor[ti] if math.isfinite(factor[ti]) else 1.0
        n = len(factor)
        tx, ty = t
        reach = near_m / step  # in cells
        k2 = SQRT2 - 1.0
        hypot, inf, push, pop = math.hypot, math.inf, heapq.heappush, heapq.heappop
        best = [inf] * n
        best[si] = 0.0
        came = [0] * n
        end: int | None = None
        if reach <= 0:  # h of every cell at once: the same sums as one by one, so the same floats
            hs = grid.octile_from(t, k2)
            heap = [(hs[si], 0.0, si)]
            while heap:
                _, so_far, cur = pop(heap)
                if cur == ti:
                    end = cur
                    break
                if so_far > best[cur]:
                    continue
                for off, ln in steps:
                    nxt = cur + off
                    f = factor[nxt]
                    if f == inf:
                        continue
                    c = so_far + ln * f
                    if c < best[nxt]:
                        best[nxt] = c
                        came[nxt] = cur
                        push(heap, (c + hs[nxt], c, nxt))
        else:  # h: the straight line to the reach circle, never more than the real cost (factor >= 1); each
            # cell's h kept once worked out (-1: not yet); within reach exactly where h is 0
            hc = [-1.0] * n
            hc[si] = step * max(0.0, hypot(s[0] - tx, s[1] - ty) - reach)
            heap = [(hc[si], 0.0, si)]
            while heap:
                _, so_far, cur = pop(heap)
                if cur == ti or hc[cur] == 0.0:
                    end = cur
                    break
                if so_far > best[cur]:
                    continue
                for off, ln in steps:
                    nxt = cur + off
                    f = factor[nxt]
                    if f == inf:
                        continue
                    c = so_far + ln * f
                    if c < best[nxt]:
                        best[nxt] = c
                        came[nxt] = cur
                        hn = hc[nxt]
                        if hn < 0.0:
                            ni, nj = divmod(nxt, w)
                            hn = hc[nxt] = step * max(0.0, hypot(ni - 1 - tx, nj - 1 - ty) - reach)
                        push(heap, (c + hn, c, nxt))
        if end is None:
            return None
        cells = [end]
        while cells[-1] != si:
            cells.append(came[cells[-1]])
        cells.reverse()
        mid = [grid.centre(*grid.unpad(c)) for c in cells[1:-1]]
        if end != ti:  # ended within reach: its cell, then the goal itself (to steer at)
            mid.append(grid.centre(*grid.unpad(end)))
        return [start, *mid, goal]

    def nearest(self, cost: CostMap, start: XY, accept: Callable[[int, int], bool]) -> tuple[int, int] | None:
        """The cheapest-to-reach 10 cm grid cell that `accept` takes (Dijkstra, stopping at the first one -
        no whole-map search), or None when none is reachable."""
        grid = self._grid_for(cost)
        s = grid.of(*start)
        if s is None:
            return None
        best = self._dijkstra(grid, s, accept)
        return None if isinstance(best, list) else best

    def costs(self, cost: CostMap, start: XY) -> FloatArray:
        """The route cost from start to every 10 cm grid cell (Dijkstra), inf where unreachable."""
        grid = self._grid_for(cost)
        g = grid.g
        s = grid.of(*start)
        if s is None:
            return np.full((g, g), np.inf)
        best = self._dijkstra(grid, s, None)
        assert isinstance(best, list)
        return np.array(best).reshape(g + 2, g + 2)[1:-1, 1:-1].copy()

    @staticmethod
    def _dijkstra(
        grid: _Grid, s: tuple[int, int], accept: Callable[[int, int], bool] | None
    ) -> list[float] | tuple[int, int]:
        """From cell s: the first cell `accept` takes (in cost order), else the cost to every padded cell."""
        factor, steps = grid.search_from(s), grid.steps
        inf, push, pop = math.inf, heapq.heappush, heapq.heappop
        si, w = grid.pad(*s), grid.w
        best = [inf] * len(factor)
        best[si] = 0.0
        heap = [(0.0, si)]
        while heap:
            so_far, cur = pop(heap)
            if so_far > best[cur]:
                continue
            if accept is not None:
                ci, cj = divmod(cur, w)
                if accept(ci - 1, cj - 1):
                    return ci - 1, cj - 1
            for off, ln in steps:
                nxt = cur + off
                f = factor[nxt]
                if f != inf:
                    c = so_far + ln * f
                    if c < best[nxt]:
                        best[nxt] = c
                        push(heap, (c, nxt))
        return best


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
