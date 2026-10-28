"""The room map (AI engine LLD §18.5, Plan Z4)."""

from __future__ import annotations

import time

import numpy as np
import pytest

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.room_map import RoomMap, map_image
from conqueror_ai.domain.types import Pose

NONE = np.zeros((0, 2))
POSE = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


def marks(
    floor: list[tuple[float, float]] | None = None,
    low: list[tuple[float, float]] | None = None,
    high: list[tuple[float, float]] | None = None,
    above: list[tuple[float, float]] | None = None,
) -> Marks:
    def arr(v: list[tuple[float, float]] | None) -> FloatArray:
        return NONE if not v else np.array(v, dtype=np.float64)

    return Marks(arr(floor), arr(low), arr(high), arr(above))


def at(room: RoomMap, x: float, y: float) -> tuple[int, int]:
    (c,) = room.cells(np.array([[x, y]]))
    return int(c // room.n), int(c % room.n)


def test_start_is_the_centre() -> None:
    room = RoomMap()
    assert room.n == 160
    assert at(room, 0.0, 0.0) == (80, 80)
    assert at(room, 3.99, -3.99) == (159, 0)
    assert len(room.cells(np.array([[4.01, 0.0]]))) == 0


def test_floor_point_misses_and_sees() -> None:
    room = RoomMap()
    room.update_picture(marks(floor=[(1.0, 0.0)]), 0)
    c = at(room, 1.0, 0.0)
    assert room.low[c] == pytest.approx(-0.4)
    assert room.high[c] == pytest.approx(-0.4)
    assert room.seen[c]
    assert int(room.seen.sum()) == 1
    assert int((room.low != 0).sum()) == 1


def test_low_and_high_hits() -> None:
    room = RoomMap()
    room.update_picture(marks(low=[(1.0, 0.0)], high=[(1.0, 0.5)]), 0)
    c, c2 = at(room, 1.0, 0.0), at(room, 1.0, 0.5)
    assert room.low[c] == pytest.approx(0.85)
    assert room.high[c2] == pytest.approx(0.85)
    assert room.seen[c] and room.seen[c2]
    assert room.blocked()[c] and room.blocked()[c2]


def test_above_only_sees() -> None:
    room = RoomMap()
    room.update_picture(marks(above=[(1.0, 0.0)]), 0)
    c = at(room, 1.0, 0.0)
    assert room.seen[c]
    assert room.low[c] == 0.0 and room.high[c] == 0.0


def test_one_step_per_cell_per_picture() -> None:  # Review Focus 1
    room = RoomMap()
    many = marks(low=[(1.01, 0.01)] * 5000)
    room.update_picture(many, 0)
    c = at(room, 1.01, 0.01)
    assert room.low[c] == pytest.approx(0.85)
    room.update_picture(many, 1)
    assert room.low[c] == pytest.approx(1.7)


def test_a_hit_beats_a_miss_in_the_same_picture() -> None:  # Review Focus 2
    room = RoomMap()
    room.update_picture(marks(floor=[(1.01, 0.01)] * 1000, low=[(1.02, 0.02)]), 0)
    c = at(room, 1.01, 0.01)
    assert room.low[c] == pytest.approx(0.85)
    assert room.high[c] == 0.0


def test_clamped() -> None:
    room = RoomMap()
    c = at(room, 1.0, 0.0)
    for i in range(10):
        room.update_picture(marks(low=[(1.0, 0.0)]), i)
    assert room.low[c] == pytest.approx(4.0)
    for i in range(20):
        room.update_picture(marks(floor=[(1.0, 0.0)]), 10 + i)
    assert room.low[c] == pytest.approx(-4.0)


def test_fades_only_where_seen() -> None:
    room = RoomMap()
    room.update_picture(marks(low=[(1.0, 0.0)]), 0)
    c = at(room, 1.0, 0.0)
    for i in range(50):
        room.update_picture(marks(floor=[(2.0, 0.0), (0.5, 0.5)]), 1 + i)
    assert room.blocked()[c]
    room.update_picture(marks(floor=[(1.0, 0.0)]), 100)
    room.update_picture(marks(floor=[(1.0, 0.0)]), 101)
    assert room.low[c] == pytest.approx(0.05, abs=1e-5)
    assert not room.blocked()[c]


def test_outside_ignored_edge_blocked() -> None:  # Review Focus 3
    room = RoomMap()
    room.update_picture(marks(floor=[(10.0, 0.0)], low=[(0.0, -9.0)], high=[(-5.0, 5.0)]), 0)
    assert not room.seen.any()
    assert not room.low.any() and not room.high.any()
    assert room.is_blocked(4.5, 0.0)
    assert room.is_blocked(0.0, -4.5)
    assert not room.is_blocked(0.0, 0.0)


def test_snapshot_copies_and_flags() -> None:  # Review Focus 4
    room = RoomMap()
    assert not room.snapshot(0, None, False, True).depth_ok
    room.update_picture(marks(low=[(1.0, 0.0)]), 1_000_000)
    snap = room.snapshot(1_500_000, POSE, False, True)
    assert snap.depth_ok and not snap.stale and snap.angle_trusted and snap.pose == POSE
    assert not room.snapshot(2_100_000, POSE, True, False).depth_ok
    late = room.snapshot(2_100_000, POSE, True, False)
    assert late.stale and not late.angle_trusted
    c = at(room, 1.0, 0.0)
    assert snap.blocked[c]
    for i in range(5):
        room.update_picture(marks(floor=[(1.0, 0.0)]), 2_000_000 + i)
    assert snap.blocked[c]  # a copy
    assert not snap.high[c]


def test_map_image_colours() -> None:
    room = RoomMap()
    room.update_picture(marks(floor=[(-1.0, 1.0)], low=[(1.0, 0.0)], high=[(1.0, 1.0)]), 0)
    img = map_image(room.snapshot(0, POSE, False, True))
    n = room.n
    assert img.shape == (n, n, 3) and img.dtype == np.uint8

    def px(x: float, y: float) -> tuple[int, ...]:
        ix, iy = at(room, x, y)
        return tuple(int(v) for v in img[n - 1 - ix, n - 1 - iy])

    assert px(1.0, 0.0) == (220, 40, 40)
    assert px(1.0, 1.0) == (255, 150, 0)
    assert px(-1.0, 1.0) == (110, 110, 110)
    assert px(-2.0, -2.0) == (0, 0, 0)
    assert px(0.0, 0.0) == (255, 255, 255)
    assert px(0.05, 0.05) == (255, 255, 255)  # a neighbour
    assert map_image(room.snapshot(0, None, False, True))[n - 1 - 80, n - 1 - 80].tolist() == [0, 0, 0]


def test_update_picture_is_quick() -> None:
    rng = np.random.default_rng(1)

    def pts(k: int) -> FloatArray:
        return rng.uniform(-3.9, 3.9, (k, 2))

    m = Marks(pts(20_000), pts(2_000), pts(2_000), pts(2_000))
    room = RoomMap()
    best = 1.0
    for i in range(5):
        t = time.perf_counter()
        room.update_picture(m, i)
        best = min(best, time.perf_counter() - t)
    assert best < 0.01
