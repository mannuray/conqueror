"""The room as a picture of cell kinds, and the trail (AI engine LLD §23.2)."""

from __future__ import annotations

from collections.abc import Sequence

import cv2
import numpy as np

from conqueror_ai.domain.room_map import MapSnapshot
from conqueror_ai.domain.room_picture import BEFORE, FLOOR, HIGH, LOW, UNKNOWN, Trail, crop, kinds, png

N, CELL, SIZE = 160, 0.05, 8.0


def snap(
    seen: Sequence[tuple[float, float]] = (),
    low: Sequence[tuple[float, float]] = (),
    high: Sequence[tuple[float, float]] = (),
) -> MapSnapshot:
    s = np.zeros((N, N), np.bool_)
    b = np.zeros((N, N), np.bool_)
    h = np.zeros((N, N), np.bool_)

    def ij(x: float, y: float) -> tuple[int, int]:
        return int(np.floor(x / CELL + N / 2)), int(np.floor(y / CELL + N / 2))

    for p in seen:
        s[ij(*p)] = True
    for p in low:
        b[ij(*p)] = True
    for p in high:
        b[ij(*p)] = h[ij(*p)] = True
    return MapSnapshot(0, None, CELL, b, h, s, False, True, True)


def test_kinds_values() -> None:
    sn = snap(seen=[(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)], low=[(1.0, 0.0)], high=[(2.0, 0.0)])
    before = np.zeros((N, N), np.bool_)
    before[80, 90] = True  # seen only before
    before[80, 80] = True  # also seen now: floor
    k = kinds(sn, before)
    assert k[80, 80] == FLOOR
    assert k[80, 90] == BEFORE
    assert k[100, 80] == LOW
    assert k[120, 80] == HIGH
    assert k[0, 0] == UNKNOWN


def test_crop_margin() -> None:
    k = kinds(snap(seen=[(1.0, -0.5)]), None)
    c = crop(k, CELL, SIZE)
    assert abs(c.x0 - 0.5) < 1e-9 and abs(c.y0 - (-1.0)) < 1e-9
    assert c.w == 21 and c.h == 21
    assert c.cells.shape == (21, 21) and c.cells[10, 10] == FLOOR


def test_crop_at_the_edge_stays_on_the_map() -> None:
    c = crop(kinds(snap(seen=[(3.98, 0.0)]), None), CELL, SIZE)
    assert c.x0 + c.w * CELL <= SIZE / 2 + 1e-9


def test_crop_empty() -> None:
    c = crop(kinds(snap(), None), CELL, SIZE)
    assert c.w == 0 and c.h == 0


def test_png_round_trip() -> None:
    c = crop(kinds(snap(seen=[(0.0, 0.0), (0.3, 0.2)], low=[(0.3, 0.2)]), None), CELL, SIZE)
    back = cv2.imdecode(np.frombuffer(png(c), np.uint8), cv2.IMREAD_UNCHANGED)
    assert back.shape == c.cells.shape
    assert np.array_equal(back, c.cells)


def test_trail_step() -> None:
    t = Trail()
    assert t.add(0.0, 0.0)
    assert not t.add(0.03, 0.0)
    assert t.add(0.06, 0.0)
    assert t.points() == [(0.0, 0.0), (0.06, 0.0)]


def test_trail_cap() -> None:
    t = Trail()
    for i in range(2001):
        t.add(i * 0.1, 0.0)
    p = t.points()
    assert len(p) == 2000 and p[0] == (0.1, 0.0)
