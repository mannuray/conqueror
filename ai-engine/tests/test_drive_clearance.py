"""Clearance on the room map (AI engine LLD §18.6, Plan Z5)."""

from __future__ import annotations

import numpy as np
import pytest

from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap


def snap_with(blocked: list[tuple[float, float]], seen: bool = True) -> MapSnapshot:
    """A map with the cells at these room points blocked (low), all seen (or none)."""
    room = RoomMap()
    if blocked:
        c = room.cells(np.array(blocked, dtype=np.float64))
        for _ in range(2):
            room.hit(c, "low")
    room.seen[:] = seen
    return room.snapshot(0, None, False, True)


def test_empty_map_clearance_is_distance_to_edge() -> None:
    c = ClearanceMap(snap_with([]))
    assert c.dist_m(0.0, 0.0) == pytest.approx(4.0, abs=0.06)
    assert c.dist_m(3.0, 0.0) == pytest.approx(1.0, abs=0.06)


def test_a_blocked_cell() -> None:
    c = ClearanceMap(snap_with([(1.0, 0.0)]))
    assert c.dist_m(1.5, 0.0) == pytest.approx(0.5, abs=0.05)
    assert c.dist_m(1.0, 0.0) == 0.0


def test_outside_is_zero() -> None:
    c = ClearanceMap(snap_with([]))
    assert c.dist_m(4.5, 0.0) == 0.0
    assert c.dist_m(0.0, -9.0) == 0.0
    assert c.cell_of(4.5, 0.0) is None


def test_ignore_disk() -> None:
    ball = [(1.0 + dx, dy) for dx in (-0.05, 0.0, 0.05) for dy in (-0.05, 0.0, 0.05)]
    assert ClearanceMap(snap_with(ball)).dist_m(1.0, 0.0) == 0.0
    c = ClearanceMap(snap_with(ball), ignore=(1.0, 0.0, 0.12))
    assert c.dist_m(1.0, 0.0) > 2.9


def test_unseen_counts_free() -> None:
    a = ClearanceMap(snap_with([(1.0, 0.0)], seen=True))
    b = ClearanceMap(snap_with([(1.0, 0.0)], seen=False))
    assert np.array_equal(a.dist, b.dist)
    assert a.seen_at(0.5, 0.0) and not b.seen_at(0.5, 0.0)


def test_cells_and_centres_agree() -> None:
    c = ClearanceMap(snap_with([]))
    ij = c.cell_of(1.02, -0.51)
    assert ij is not None
    x, y = c.centre(*ij)
    assert abs(x - 1.02) <= 0.025 and abs(y + 0.51) <= 0.025
