"""The cost map (AI engine LLD §18.13.1, Plan Z6): every 5 cm cell has a price. Known obstacles (and the map's
edge) are lethal; near them the price rises; floor never seen costs something but is never forbidden; seen
open floor is free. Nothing is grown into unseen space (design-notes §166)."""

from __future__ import annotations

import math
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.drive.cost_settings import CostSettings
from conqueror_ai.domain.room_map import BoolArray, MapSnapshot

__all__ = ["MAX_COST", "CostMap", "CostSettings"]

MAX_COST = 0.95  # a free cell never costs more than this; lethal is 1


class CostMap:
    def __init__(
        self,
        snap: MapSnapshot,
        s: CostSettings | None = None,
        ignore: tuple[float, float, float] | None = None,
    ) -> None:
        """ignore = (x, y, r): marks within r of (x, y) count as free (the chase's ball sits in its own)."""
        self.settings = s or CostSettings()
        self.n = snap.blocked.shape[0]
        self.cell_m = snap.cell_m
        self.half_m = self.n * self.cell_m / 2
        lethal = snap.blocked.copy()
        if ignore is not None:
            x, y, r = ignore
            c = (np.arange(self.n) + 0.5) * self.cell_m - self.half_m
            lethal &= np.hypot(c[:, None] - x, c[None, :] - y) > r
        self.lethal: BoolArray = lethal
        self.seen: BoolArray = snap.seen
        self.known_free: BoolArray = snap.seen & ~lethal
        free = np.zeros((self.n + 2, self.n + 2), np.uint8)  # a lethal border: the map's edge
        free[1:-1, 1:-1] = ~lethal
        d = cast("NDArray[np.float32]", cv2.distanceTransform(free, cv2.DIST_L2, cv2.DIST_MASK_PRECISE))
        self.dist: FloatArray = d[1:-1, 1:-1].astype(np.float64) * self.cell_m

    def cell_of(self, x: float, y: float) -> tuple[int, int] | None:
        ix, iy = math.floor((x + self.half_m) / self.cell_m), math.floor((y + self.half_m) / self.cell_m)
        return (ix, iy) if 0 <= ix < self.n and 0 <= iy < self.n else None

    def centre(self, ix: int, iy: int) -> tuple[float, float]:
        return ((ix + 0.5) * self.cell_m - self.half_m, (iy + 0.5) * self.cell_m - self.half_m)

    def dist_m(self, x: float, y: float) -> float:
        """Metres to the nearest known obstacle (cell centre to cell centre); 0 outside the map."""
        ij = self.cell_of(x, y)
        return 0.0 if ij is None else float(self.dist[ij])

    def seen_at(self, x: float, y: float) -> bool:
        ij = self.cell_of(x, y)
        return ij is not None and bool(self.seen[ij])

    def _cells(self, xy: FloatArray) -> tuple[NDArray[np.int64], NDArray[np.int64], NDArray[np.bool_]]:
        ij = np.floor((xy + self.half_m) / self.cell_m).astype(np.int64)
        inside = cast("NDArray[np.bool_]", np.all((ij >= 0) & (ij < self.n), axis=-1))
        ij = cast("NDArray[np.int64]", np.where(inside[..., None], ij, 0))
        return ij[..., 0], ij[..., 1], inside

    def dist_many(self, xy: FloatArray) -> FloatArray:
        """dist_m for an (..., 2) array of points (the body's outline)."""
        i, j, inside = self._cells(xy)
        return np.where(inside, self.dist[i, j], 0.0)

    def outline_cost(self, xy: FloatArray) -> FloatArray:
        """The fan's reading at points of the body's outline: the near cost by the point's distance (zero at
        near_out_m, full at keep_out_m) plus the unknown cost, capped at MAX_COST; 1 in an obstacle or
        outside."""
        s = self.settings
        i, j, inside = self._cells(xy)
        d = np.where(inside, self.dist[i, j], 0.0)
        near = s.near_cost * np.clip((s.near_out_m - d) / (s.near_out_m - s.keep_out_m), 0.0, 1.0)
        unknown = np.where(inside & ~self.seen[i, j], s.unknown_cost, 0.0)
        cost = np.minimum(near + unknown, MAX_COST)
        return np.where(inside & ~self.lethal[i, j], cost, 1.0)

    def centre_cost(self, half_w: float) -> FloatArray:
        """Routes' reading, per cell for the tank's centre: the near cost zero at near_c_m, full at the body's
        half-width, plus unknown, capped; 1 where lethal."""
        s = self.settings
        near = s.near_cost * np.clip((s.near_c_m - self.dist) / (s.near_c_m - half_w), 0.0, 1.0)
        cost = np.minimum(near + np.where(self.seen, 0.0, s.unknown_cost), MAX_COST)
        return np.where(self.lethal, 1.0, cost)
