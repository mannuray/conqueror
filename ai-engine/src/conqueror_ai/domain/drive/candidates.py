"""Find's candidates (design-notes §189, AI engine LLD §18.13.23): sightings of the thing sought (from the
maybe bar up) and its remembered spots, each checked from near before unlooked floor - confirmed, the chase
takes over; not, it is ruled out for the run."""

from __future__ import annotations

import itertools
import math
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Literal

from conqueror_ai.domain.drive.chase import TARGET_R_M
from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.route import XY, RoutePlanner
from conqueror_ai.domain.memory import Remembered
from conqueror_ai.domain.reach import SAME_SPOT_M, SURE_REACH_M
from conqueror_ai.domain.room_map import MapSnapshot


@dataclass
class Candidate:
    x_m: float
    y_m: float
    state: Literal["pending", "ruled_out"] = "pending"
    remembered: Remembered | None = None  # from the memory of things: not there -> marked gone
    tries: int = 0  # checks that could not get near it (no way, no progress, out of time)

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
        """A sighting: the nearby candidate moves to it; near a ruled-out one ignored; else a new one."""
        c = self._near(spot)
        if c is None:
            self._all.append(Candidate(*spot))
        elif c.state == "pending":
            c.x_m, c.y_m = spot

    def seed(self, remembered: Sequence[Remembered]) -> None:
        for r in remembered:
            if self._near((r.x_m, r.y_m)) is None:
                self._all.append(Candidate(r.x_m, r.y_m, remembered=r))

    def pending(self) -> list[Candidate]:
        return [c for c in self._all if c.state == "pending"]

    def rule_out(self, c: Candidate) -> None:
        c.state = "ruled_out"

    def route(
        self,
        c: Candidate,
        at: XYTh,
        snap: MapSnapshot,
        routes: RoutePlanner,
        area: tuple[float, float, float] | None,
        settings: CostSettings | None = None,
    ) -> list[XY] | None:
        """A way to within SURE_REACH_M of it, its own marks within TARGET_R_M set aside (the chase's rule);
        None outside the play area or with no way."""
        if area is not None and math.dist(c.spot, area[:2]) > area[2]:
            return None
        cost = CostMap(snap, settings, ignore=(c.x_m, c.y_m, TARGET_R_M), area=area)
        return routes.route(cost, (at[0], at[1]), c.spot, goal_ok=True, near_m=SURE_REACH_M)

    def next(
        self,
        at: XYTh,
        snap: MapSnapshot,
        routes: RoutePlanner,
        area: tuple[float, float, float] | None,
        settings: CostSettings | None = None,
    ) -> Candidate | None:
        """The pending candidate with the shortest way to within SURE_REACH_M of it, or None."""
        best: tuple[float, Candidate] | None = None
        for c in self.pending():
            way = self.route(c, at, snap, routes, area, settings)
            if way is None:
                continue
            n = sum(math.dist(a, b) for a, b in itertools.pairwise(way))
            if best is None or n < best[0]:
                best = (n, c)
        return None if best is None else best[1]
