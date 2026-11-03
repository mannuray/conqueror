"""One snapshot's cost maps (AI engine LLD §19.3, review F6): built once per snapshot and key, the play area
always in them - a skill can't forget it."""

from __future__ import annotations

from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.room_map import MapSnapshot


class CostMaps:
    def __init__(
        self, snap: MapSnapshot, settings: CostSettings | None, area: tuple[float, float, float] | None
    ) -> None:
        """area = (x, y, r): the play area (None: anywhere)."""
        self.snap = snap
        self.settings = settings
        self._area = area
        self._base: CostMap | None = None
        self._ignoring: dict[tuple[float, float, float], CostMap] = {}

    @property
    def area(self) -> tuple[float, float, float] | None:
        return self._area

    def base(self) -> CostMap:
        if self._base is None:
            self._base = CostMap(self.snap, self.settings, area=self._area)
        return self._base

    def ignoring(self, x: float, y: float, r: float) -> CostMap:
        """Marks within r of (x, y) set aside: a thing's own marks (the chase's target, a candidate)."""
        key = (round(x, 2), round(y, 2), r)
        if key not in self._ignoring:
            self._ignoring[key] = CostMap(self.snap, self.settings, ignore=(x, y, r), area=self._area)
        return self._ignoring[key]
