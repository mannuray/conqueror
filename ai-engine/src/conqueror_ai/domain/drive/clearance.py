"""Clearance on the room map (AI engine LLD §18.6): metres from each 5 cm cell to the nearest blocked cell or
the map's edge (a wall), worked out once per snapshot. Unseen cells count as free here: the camera sees them
before the tank gets there (spec 9 §4.2); routes pay extra for them instead."""

from __future__ import annotations

import math
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.room_map import MapSnapshot


class ClearanceMap:
    def __init__(self, snap: MapSnapshot, ignore: tuple[float, float, float] | None = None) -> None:
        """ignore = (x, y, r): blocked cells within r of (x, y) count as free (the chase's target sits in its
        own marks)."""
        self.n = snap.blocked.shape[0]
        self.cell_m = snap.cell_m
        self.half_m = self.n * self.cell_m / 2
        blocked = snap.blocked.copy()
        if ignore is not None:
            x, y, r = ignore
            c = (np.arange(self.n) + 0.5) * self.cell_m - self.half_m
            blocked &= np.hypot(c[:, None] - x, c[None, :] - y) > r
        free = np.zeros((self.n + 2, self.n + 2), np.uint8)  # a blocked border: the edge is a wall
        free[1:-1, 1:-1] = ~blocked
        d = cast("NDArray[np.float32]", cv2.distanceTransform(free, cv2.DIST_L2, cv2.DIST_MASK_PRECISE))
        self.dist: FloatArray = d[1:-1, 1:-1].astype(np.float64) * self.cell_m
        self.seen = snap.seen

    def cell_of(self, x: float, y: float) -> tuple[int, int] | None:
        ix, iy = math.floor((x + self.half_m) / self.cell_m), math.floor((y + self.half_m) / self.cell_m)
        return (ix, iy) if 0 <= ix < self.n and 0 <= iy < self.n else None

    def centre(self, ix: int, iy: int) -> tuple[float, float]:
        return ((ix + 0.5) * self.cell_m - self.half_m, (iy + 0.5) * self.cell_m - self.half_m)

    def dist_m(self, x: float, y: float) -> float:
        """Metres to the nearest obstacle; 0 outside the map."""
        ij = self.cell_of(x, y)
        return 0.0 if ij is None else float(self.dist[ij])

    def seen_at(self, x: float, y: float) -> bool:
        ij = self.cell_of(x, y)
        return ij is not None and bool(self.seen[ij])
