"""The cost map (AI engine LLD §18.13.1, Plan Z6)."""

from __future__ import annotations

import numpy as np
import pytest

from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap

HALF_W = 0.115


def snap_with(marks: list[tuple[float, float]], seen: bool = True) -> MapSnapshot:
    """The cells at these room points marked low (twice), every cell seen (or none)."""
    room = RoomMap()
    if marks:
        c = room.cells(np.array(marks, dtype=np.float64))
        room.hit(c, "low")
        room.hit(c, "low")
    room.seen[:] = seen
    return room.snapshot(0, None, False, True)


def at(cost: CostMap, x: float, y: float) -> float:
    return float(cost.outline_cost(np.array([[x, y]]))[0])


def test_lethal_is_marks_and_the_border() -> None:
    c = CostMap(snap_with([(1.0, 0.0)]))
    assert c.lethal[c.cell_of(1.0, 0.0)] and c.dist_m(1.0, 0.0) == 0.0
    assert c.dist_m(3.0, 0.0) == pytest.approx(1.0, abs=0.06)  # the map's edge
    assert c.dist_m(4.5, 0.0) == 0.0 and c.cell_of(4.5, 0.0) is None
    assert c.dist_many(np.array([[4.5, 0.0], [1.0, 0.0]])).tolist() == [0.0, 0.0]


def test_outline_cost_by_distance() -> None:
    c = CostMap(snap_with([(1.025, 0.025)]))  # one cell, centre (1.025, 0.025)
    assert at(c, 1.025 - 0.20, 0.025) == 0.0
    assert at(c, 1.025 - 0.075, 0.025) == pytest.approx(0.8)
    assert at(c, 1.025 - 0.15, 0.025) == pytest.approx(0.8 * (0.20 - 0.15) / (0.20 - 0.075))
    assert at(c, 1.025, 0.025) == 1.0
    assert at(c, 9.0, 0.0) == 1.0  # outside the map


def test_unknown_adds_and_caps() -> None:
    c = CostMap(snap_with([(1.025, 0.025)], seen=False))
    assert at(c, -1.0, 0.0) == pytest.approx(0.3)  # open, never seen
    assert at(c, 1.025 - 0.075, 0.025) == pytest.approx(0.95)  # 0.8 + 0.3, capped


def test_centre_cost_for_routes() -> None:
    c = CostMap(snap_with([(1.025, 0.025)]))
    grid = c.centre_cost(HALF_W)

    def cell(x: float) -> float:
        ij = c.cell_of(x, 0.025)
        assert ij is not None
        return float(grid[ij])

    assert cell(1.025 - 0.40) == 0.0
    assert cell(1.025 - 0.10) == pytest.approx(0.8)  # nearer than the half-width: full
    assert cell(1.025 - 0.25) == pytest.approx(0.8 * (0.40 - 0.25) / (0.40 - HALF_W))
    assert cell(1.025) == 1.0


def test_known_free_is_seen_and_not_lethal() -> None:
    c = CostMap(snap_with([(1.0, 0.0)]))
    assert c.known_free[c.cell_of(0.0, 0.0)] and not c.known_free[c.cell_of(1.0, 0.0)]
    u = CostMap(snap_with([], seen=False))
    assert not u.known_free[u.cell_of(0.0, 0.0)] and u.seen_at(0.0, 0.0) is False


def test_ignore_disk() -> None:
    ball = [(1.0 + dx, dy) for dx in (-0.05, 0.0, 0.05) for dy in (-0.05, 0.0, 0.05)]
    assert CostMap(snap_with(ball)).dist_m(1.0, 0.0) == 0.0
    assert CostMap(snap_with(ball), ignore=(1.0, 0.0, 0.12)).dist_m(1.0, 0.0) > 2.9


def test_play_area_outside_is_a_wall() -> None:
    """--area (design-notes §174): outside the circle round the start is lethal, like a wall."""
    c = CostMap(snap_with([]), area=(0.5, 0.0, 1.0))
    assert not c.lethal[c.cell_of(1.4, 0.0)] and c.lethal[c.cell_of(1.6, 0.0)]
    assert c.lethal[c.cell_of(-0.6, 0.0)] and not c.known_free[c.cell_of(0.5, 1.1)]
    assert c.dist_m(0.5, 0.0) == pytest.approx(1.0, abs=0.06)
    assert CostMap(snap_with([])).lethal.sum() == 0


def test_nothing_grows_into_unknown() -> None:
    """A seen mark beside unseen floor: the unseen floor behind it is dear, never lethal (§18.13.1)."""
    room = RoomMap()
    c = room.cells(np.array([(1.025, 0.025)], dtype=np.float64))
    room.hit(c, "low")
    room.hit(c, "low")
    room.seen.reshape(-1)[c] = True
    cost = CostMap(room.snapshot(0, None, False, True))
    assert not cost.lethal[cost.cell_of(1.125, 0.025)]
    assert cost.dist_m(1.125, 0.025) == pytest.approx(0.10)
    assert 0.3 < at(cost, 1.125, 0.025) < 1.0


def test_settings_defaults() -> None:
    s = CostSettings()
    assert (s.keep_out_m, s.near_out_m, s.near_c_m, s.near_cost, s.unknown_cost) == (
        0.075,
        0.20,
        0.40,
        0.8,
        0.3,
    )
    assert (s.cost_w, s.route_cost_w, s.stuck_s, s.back_m) == (4.0, 6.0, 3.0, 0.20)
