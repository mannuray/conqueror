"""Find's candidates (design-notes §189, AI engine LLD §18.13.23): sightings of the thing sought (from the
maybe bar up) and its remembered spots, each checked from near before unlooked floor - confirmed, the chase
takes over; not, it is ruled out - opened again when seen again, at most REOPEN_MAX times (LLD §24)."""

from __future__ import annotations

import itertools
import math
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Literal

from conqueror_ai.domain.drive.chase import TARGET_R_M
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.route import XY, RoutePlanner
from conqueror_ai.domain.memory import Remembered
from conqueror_ai.domain.reach import CHECK_CLOSE_M, SAME_SPOT_M

ROUTE_TRIES = 3  # pending candidates routed per try, nearest first (review F6)
REOPEN_MAX = 2  # a ruled-out spot seen again is checked again this many times at most (LLD §24 A)


@dataclass
class Candidate:
    x_m: float
    y_m: float
    state: Literal["pending", "ruled_out"] = "pending"
    remembered: Remembered | None = None  # from the memory of things: not there -> marked gone
    tries: int = 0  # checks that could not get near it (no way, no progress, out of time)
    reopens: int = 0  # times a sighting opened it again after it was ruled out (LLD §24 A)

    @property
    def spot(self) -> XY:
        return self.x_m, self.y_m


class Candidates:
    def __init__(self) -> None:
        self._all: list[Candidate] = []

    def _near(self, spot: XY) -> Candidate | None:
        near = [c for c in self._all if math.dist(c.spot, spot) <= SAME_SPOT_M]
        return min(near, key=lambda c: math.dist(c.spot, spot), default=None)

    def add(self, spot: XY) -> None:
        """A sighting: the nearby candidate moves to it; a ruled-out one opens again, at most REOPEN_MAX times
        (one glimpse always gets a look, §215), then ignored; else a new one."""
        c = self._near(spot)
        if c is None:
            self._all.append(Candidate(*spot))
        elif c.state == "pending":
            c.x_m, c.y_m = spot
        elif c.reopens < REOPEN_MAX:
            c.state, c.reopens = "pending", c.reopens + 1
            c.x_m, c.y_m = spot

    def seed(self, remembered: Sequence[Remembered]) -> None:
        for r in remembered:
            if self._near((r.x_m, r.y_m)) is None:
                self._all.append(Candidate(r.x_m, r.y_m, remembered=r))

    def pending(self) -> list[Candidate]:
        return [c for c in self._all if c.state == "pending"]

    def ruled_out(self) -> list[XY]:
        return [c.spot for c in self._all if c.state == "ruled_out"]

    def rule_out(self, c: Candidate) -> None:
        c.state = "ruled_out"

    def route(self, c: Candidate, at: XYTh, costs: CostMaps, routes: RoutePlanner) -> list[XY] | None:
        """A way to within CHECK_CLOSE_M of it (the check looks from there, §24 C), its own marks within
        TARGET_R_M set aside (the chase's rule); None outside the play area or with no way."""
        area = costs.area
        if area is not None and math.dist(c.spot, area[:2]) > area[2]:
            return None
        cost = costs.ignoring(c.x_m, c.y_m, TARGET_R_M)
        return routes.route(cost, (at[0], at[1]), c.spot, goal_ok=True, near_m=CHECK_CLOSE_M)

    def next(
        self, at: XYTh, costs: CostMaps, routes: RoutePlanner, limit: int | None = ROUTE_TRIES
    ) -> Candidate | None:
        """Of the ROUTE_TRIES pending candidates nearest in a straight line, the one with the shortest way to
        within CHECK_CLOSE_M of it, or None (the rest wait for a later try: a route takes 6-21 ms, F6).
        limit None: every pending one (once, before a find gives up - Z7 final review I1)."""
        best: tuple[float, Candidate] | None = None
        nearest = sorted(self.pending(), key=lambda c: math.dist(c.spot, at[:2]))[:limit]
        for c in nearest:
            way = self.route(c, at, costs, routes)
            if way is None:
                continue
            n = sum(math.dist(a, b) for a, b in itertools.pairwise(way))
            if best is None or n < best[0]:
                best = (n, c)
        return None if best is None else best[1]
