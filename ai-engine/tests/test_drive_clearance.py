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
    a = ClearanceMap(snap_with([], seen=True))
    b = ClearanceMap(snap_with([], seen=False))
    assert np.array_equal(a.dist, b.dist)  # open unseen space is free
    a = ClearanceMap(snap_with([(1.0, 0.0)], seen=True))
    assert a.seen_at(0.5, 0.0) and not b.seen_at(0.5, 0.0)


def test_cells_and_centres_agree() -> None:
    c = ClearanceMap(snap_with([]))
    ij = c.cell_of(1.02, -0.51)
    assert ij is not None
    x, y = c.centre(*ij)
    assert abs(x - 1.02) <= 0.025 and abs(y + 0.51) <= 0.025


def test_unseen_next_to_an_obstacle_counts_as_obstacle() -> None:
    """A box's far side is unseen: the obstacle may go on there, so unseen cells within GROW_UNSEEN_M of a
    marked one count as blocked; unseen open space far from anything stays free (Plan Z5 sim finding)."""
    room = RoomMap()
    face = room.cells(np.array([(0.98, float(y)) for y in np.arange(-0.2, 0.21, 0.025)], dtype=np.float64))
    room.hit(face, "low")
    room.hit(face, "low")
    room.seen[:] = True  # the face seen (inside its 5 cm cell, clear of the unseen area behind)
    pts = [(float(x), float(y)) for x in np.arange(1.05, 1.6, 0.025) for y in np.arange(-0.4, 0.41, 0.025)]
    behind = room.cells(np.array(pts, dtype=np.float64))
    room.seen.reshape(-1)[behind] = False
    c = ClearanceMap(room.snapshot(0, None, False, True))
    assert c.dist_m(1.2, 0.0) == 0.0  # unseen, 20 cm behind the face: part of the obstacle
    assert c.dist_m(1.5, 0.35) > 0.0  # unseen but beyond 30 cm of anything marked
    assert c.dist_m(0.6, 0.0) == pytest.approx(0.4, abs=0.06)  # in front, unchanged


def test_sonar_only_marks_dont_grow_into_unseen_space() -> None:
    """A sonar arc marks unseen cells low (it never marks seen): growing those 30 cm walled off the simulated
    ball (Plan Z5). Only marks the camera has seen grow."""
    room = RoomMap()
    arc = room.cells(np.array([(1.0, float(y)) for y in np.arange(-0.2, 0.21, 0.025)], dtype=np.float64))
    room.hit(arc, "low")
    room.hit(arc, "low")  # unseen: as the sonar leaves it
    c = ClearanceMap(room.snapshot(0, None, False, True))
    assert c.dist_m(1.0, 0.0) == 0.0
    assert c.dist_m(1.2, 0.0) == pytest.approx(0.2, abs=0.06)  # not grown


def test_many_points_at_once_match_one_at_a_time() -> None:
    """The body's outline is checked at many points per move (design-notes §164): one call for all of them."""
    c = ClearanceMap(snap_with([(1.0, 0.0), (-0.5, 0.7)]))
    pts = np.array([(0.0, 0.0), (1.02, 0.01), (0.6, -0.3), (4.5, 0.0), (0.0, -9.0), (-3.99, 3.99)])
    assert c.dist_many(pts).tolist() == [c.dist_m(float(x), float(y)) for x, y in pts]
