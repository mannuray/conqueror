"""The distance sensor on the room map (AI engine LLD §18.5, spec 9 §5, Plan Z4)."""

from __future__ import annotations

import math

import numpy as np
import pytest

from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.sonar_model import SonarModel
from conqueror_ai.domain.types import Pose

START = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


def cell(room: RoomMap, x: float, y: float) -> tuple[int, int]:
    (c,) = room.cells(np.array([[x, y]]))
    return int(c // room.n), int(c % room.n)


def ahead(d: float, deg: float, x0: float = 0.13) -> tuple[float, float]:
    a = math.radians(deg)
    return x0 + d * math.cos(a), d * math.sin(a)


def test_echo_marks_the_arc_low() -> None:
    room = RoomMap()
    assert SonarModel().update(room, START, 1.0, 0)
    assert room.low[cell(room, 1.13, 0.0)] > 0
    assert room.low[cell(room, *ahead(1.0, 10.0))] > 0
    assert room.low[cell(room, *ahead(1.0, 20.0))] == 0
    assert not room.high.any()


def test_cone_cleared_low_only() -> None:
    room = RoomMap()
    (c,) = room.cells(np.array([[0.63, 0.0]]))
    room.hit(np.array([c]), "low")
    room.hit(np.array([c]), "high")
    SonarModel().update(room, START, 1.0, 0)
    ij = cell(room, 0.63, 0.0)
    assert room.low[ij] == pytest.approx(0.85 - 0.4)
    assert room.high[ij] == pytest.approx(0.85)  # the sonar never touches high


@pytest.mark.parametrize("d", [None, 4.0, 5.0])
def test_no_echo_changes_nothing(d: float | None) -> None:
    room = RoomMap()
    assert not SonarModel().update(room, START, d, 0)
    assert not room.low.any() and not room.high.any() and not room.seen.any()


def test_ten_per_second() -> None:
    s, room = SonarModel(), RoomMap()
    assert [s.update(room, START, 1.0, t) for t in (0, 50_000, 100_000)] == [True, False, True]
    s2 = SonarModel()
    assert not s2.update(room, START, None, 0)
    assert s2.update(room, START, 1.0, 50_000)  # no echo doesn't use up the slot


def test_turns_with_the_pose() -> None:
    room = RoomMap()
    SonarModel().update(room, Pose(0, 1.0, 1.0, math.pi / 2, 0.0, 0.0), 1.0, 0)
    assert room.low[cell(room, 1.0, 2.13)] > 0
    assert room.low[cell(room, 2.13, 1.0)] == 0


def test_very_near_echo() -> None:  # Review Focus 5
    room = RoomMap()
    assert SonarModel().update(room, START, 0.05, 0)
    assert room.low[cell(room, 0.18, 0.0)] > 0
    assert not (room.low < 0).any()


def test_beyond_the_map() -> None:  # Review Focus 3
    room = RoomMap()
    SonarModel().update(room, Pose(0, 3.5, 0.0, 0.0, 0.0, 0.0), 1.0, 0)  # the arc at x 4.63: outside
    assert room.low[cell(room, 3.8, 0.0)] < 0  # the cone inside the map is cleared
    assert not (room.low > 0).any()


def test_arc_has_no_gaps() -> None:
    room = RoomMap()
    SonarModel().update(room, START, 3.5, 0)
    inside = 0
    for deg in np.arange(-12.0, 12.0001, 0.0164):  # ~1 mm along the arc
        x, y = ahead(3.5, float(deg))
        fx, fy = ((x + 4.0) / 0.05) % 1, ((y + 4.0) / 0.05) % 1
        if min(fx, 1 - fx, fy, 1 - fy) < 0.1:
            continue  # within 5 mm of a cell's edge: a corner graze, not a gap in a 24 deg beam
        inside += 1
        assert room.low[cell(room, x, y)] > 0, deg
    assert inside > 1000


def test_does_not_mark_seen() -> None:
    room = RoomMap()
    SonarModel().update(room, START, 1.0, 0)
    assert not room.seen.any()  # only the camera turns unseen into seen (spec 9 §5)
