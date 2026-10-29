"""Clearance on the room map (AI engine LLD §18.6): metres from each 5 cm cell to the nearest blocked cell or
the map's edge (a wall), worked out once per snapshot. Unseen open space counts as free here: the camera sees
it before the tank gets there (spec 9 §4.2); routes pay extra for it instead. But unseen cells within
GROW_UNSEEN_M of a seen mark count as that obstacle: a box's far side is unseen, and in the simulated room
the tank cut round such a corner inside the camera's blind zone (the map never invents free space)."""

from __future__ import annotations

import math
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.room_map import MapSnapshot

GROW_UNSEEN_M = 0.30  # an obstacle may go on this far into unseen space


class ClearanceMap:
    def __init__(
        self, snap: MapSnapshot, ignore: tuple[float, float, float] | None = None, grow: bool = True
    ) -> None:
        """ignore = (x, y, r): blocked cells within r of (x, y) count as free (the chase's target sits in its
        own marks). grow: unseen space next to seen marks counts as the thing - for the fan; routes and goals
        leave it out (review I3: it closed doorways)."""
        self.n = snap.blocked.shape[0]
        self.cell_m = snap.cell_m
        self.half_m = self.n * self.cell_m / 2
        blocked = snap.blocked.copy()
        if ignore is not None:
            x, y, r = ignore
            c = (np.arange(self.n) + 0.5) * self.cell_m - self.half_m
            blocked &= np.hypot(c[:, None] - x, c[None, :] - y) > r
        # the marks alone, never grown: a body standing in grown unseen space reads 0 everywhere on the grown
        # map, so its moves are checked against these too (design-notes §164)
        self.marks_dist: FloatArray = _dist(blocked) * self.cell_m
        seen_marks = blocked & snap.seen  # the camera's marks; a sonar arc (never seen) doesn't grow
        if grow and seen_marks.any():
            to_mark = cast(
                "NDArray[np.float32]", cv2.distanceTransform((~seen_marks).astype(np.uint8), cv2.DIST_L2, 5)
            )
            blocked |= ~snap.seen & (to_mark * self.cell_m <= GROW_UNSEEN_M)
        self.dist: FloatArray = _dist(blocked) * self.cell_m
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

    def dist_many(self, xy: FloatArray, marks_only: bool = False) -> FloatArray:
        """dist_m for an (..., 2) array of points at once (the body's outline, design-notes §164); marks_only:
        to the marks themselves (no unseen space grown)."""
        dist = self.marks_dist if marks_only else self.dist
        ij = np.floor((xy + self.half_m) / self.cell_m).astype(np.int64)
        inside = ((ij >= 0) & (ij < self.n)).all(axis=-1)
        out = np.zeros(xy.shape[:-1])
        out[inside] = dist[ij[inside][:, 0], ij[inside][:, 1]]
        return out

    def seen_at(self, x: float, y: float) -> bool:
        ij = self.cell_of(x, y)
        return ij is not None and bool(self.seen[ij])


def _dist(blocked: NDArray[np.bool_]) -> FloatArray:
    """Cells to the nearest blocked cell, with a blocked border (the map's edge is a wall)."""
    n = blocked.shape[0]
    free = np.zeros((n + 2, n + 2), np.uint8)
    free[1:-1, 1:-1] = ~blocked
    d = cast("NDArray[np.float32]", cv2.distanceTransform(free, cv2.DIST_L2, cv2.DIST_MASK_PRECISE))
    return d[1:-1, 1:-1].astype(np.float64)
