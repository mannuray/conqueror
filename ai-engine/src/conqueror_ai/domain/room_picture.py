"""The room as the /ai page's ROOM tab gets it (AI engine LLD §23.2): one byte per 5 cm cell saying what the
cell is - unknown, seen only on an earlier home run, floor, low, high - cropped to what is known plus 0.5 m,
as a grey PNG the page paints in its own look; and the tank's trail. Pure: no I/O."""

from __future__ import annotations

import math
from dataclasses import dataclass

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.room_map import MapSnapshot

UNKNOWN, BEFORE, FLOOR, LOW, HIGH = 0, 1, 2, 3, 4
KINDS = (UNKNOWN, BEFORE, FLOOR, LOW, HIGH)
MARGIN_M = 0.5
TRAIL_STEP_M = 0.05
TRAIL_CAP = 2000


def kinds(snap: MapSnapshot, seen_before: NDArray[np.bool_] | None) -> NDArray[np.uint8]:
    """Each cell's kind, indexed [ix, iy] as the map; high wins over low, an obstacle over floor."""
    k = np.zeros(snap.seen.shape, np.uint8)
    if seen_before is not None:
        k[seen_before] = BEFORE
    k[snap.seen] = FLOOR
    k[snap.blocked] = LOW
    k[snap.high] = HIGH
    return k


@dataclass(frozen=True)
class Crop:
    x0: float  # the room point of the crop's corner with the smallest x and y
    y0: float
    w: int  # cells along x
    h: int  # cells along y
    cells: NDArray[np.uint8]  # cells[i, j]: the cell at x0 + i * cell, y0 + j * cell


def crop(k: NDArray[np.uint8], cell_m: float, size_m: float, margin_m: float = MARGIN_M) -> Crop:
    """The known cells and margin_m round them, kept on the map; nothing known: an empty crop."""
    ix, iy = np.nonzero(k)
    if ix.size == 0:
        return Crop(0.0, 0.0, 0, 0, np.zeros((0, 0), np.uint8))
    n = k.shape[0]
    m = round(margin_m / cell_m)
    i0, i1 = max(0, int(ix.min()) - m), min(n, int(ix.max()) + m + 1)
    j0, j1 = max(0, int(iy.min()) - m), min(n, int(iy.max()) + m + 1)
    half = size_m / 2
    cells = np.ascontiguousarray(k[i0:i1, j0:j1])
    return Crop(i0 * cell_m - half, j0 * cell_m - half, i1 - i0, j1 - j0, cells)


def png(c: Crop) -> bytes:
    """The crop as a grey PNG: row = x index, column = y index."""
    ok, buf = cv2.imencode(".png", c.cells)
    if not ok:
        raise ValueError("PNG encode failed")
    return buf.tobytes()


class Trail:
    """Where the tank has been: a point each step_m moved, the newest cap kept."""

    def __init__(self, step_m: float = TRAIL_STEP_M, cap: int = TRAIL_CAP) -> None:
        self.step_m, self.cap = step_m, cap
        self._points: list[tuple[float, float]] = []

    def add(self, x: float, y: float) -> bool:
        """Keep (x, y) if it is step_m from the last point; True if kept."""
        if not (math.isfinite(x) and math.isfinite(y)):
            return False
        if self._points and math.dist(self._points[-1], (x, y)) < self.step_m:
            return False
        self._points.append((round(x, 2), round(y, 2)))
        if len(self._points) > self.cap:
            del self._points[0]
        return True

    def points(self) -> list[tuple[float, float]]:
        return list(self._points)
