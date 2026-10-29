"""One body rule (AI engine LLD §18.13.2, Plan Z6): no outline point nearer than the keep-out to a known
obstacle, or nearer than it already is."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import STOP, Command, roll_out
from conqueror_ai.domain.drive.footprint import Footprint
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.room_map import RoomMap

LIM = Limits()
F = Footprint(LIM)
AT = (0.0, 0.0, 0.0)


def area(x0: float, y0: float, x1: float, y1: float) -> list[tuple[float, float]]:
    return [
        (float(x), float(y)) for x in np.arange(x0, x1 + 1e-9, 0.025) for y in np.arange(y0, y1 + 1e-9, 0.025)
    ]


def cost_with(marks: list[tuple[float, float]], unseen: list[tuple[float, float]] | None = None) -> CostMap:
    room = RoomMap()
    room.seen[:] = True
    if unseen:
        room.seen.reshape(-1)[room.cells(np.array(unseen, dtype=np.float64))] = False
    if marks:
        c = room.cells(np.array(marks, dtype=np.float64))
        room.hit(c, "low")
        room.hit(c, "low")
        room.seen.reshape(-1)[c] = True
    return CostMap(room.snapshot(0, None, False, True))


def test_the_outline_and_its_moves() -> None:
    assert F.outline[:, 0].max() == LIM.body_front_m and F.outline[:, 0].min() == -LIM.body_rear_m
    assert np.abs(F.outline[:, 1]).max() == LIM.body_half_w_m
    spin = F.spin_poses(AT, left=True)
    assert len(spin) == 18 and math.degrees(spin[-1][2]) == 90.0
    back = F.back_poses(AT, 0.2)
    assert back[-1][0] == -0.2 and len(back) == 8


def test_into_the_keep_out_is_refused_beside_it_is_dear() -> None:
    wall = cost_with(area(0.30, -1.5, 0.35, 1.5))  # its cells from 0.25 m (0.30 sits on a cell edge)
    assert not F.allowed(wall, AT, roll_out(AT, STOP, Command(25.0, 0.0), LIM))
    short = [(0.025, 0.0, 0.0), (0.05, 0.0, 0.0)]  # 5 cm: the front's cell 10 cm from the wall's
    assert F.allowed(wall, AT, short)
    assert F.highest_cost(wall, AT, short) > 0.5


def bed() -> CostMap:
    """Floor run 2: the bed's side panel to the right, its leg ahead-right (cells as the map had them)."""
    return cost_with(area(-0.6, -0.40, 0.10, -0.175) + area(0.075, -0.40, 0.30, -0.15))


def test_run_2_beside_the_bed() -> None:
    b = bed()
    assert not F.allowed(b, AT, F.spin_poses(AT, left=True))  # the rear swings into the panel
    assert not F.allowed(b, AT, F.spin_poses(AT, left=False))  # the front into the leg
    assert not F.allowed(b, AT, roll_out(AT, STOP, Command(25.0, 0.0), LIM))  # past the leg
    assert F.allowed(b, AT, F.back_poses(AT, 0.2))  # back the way it came


def test_run_3_unknown_round_the_tank() -> None:
    c = cost_with(area(0.40, 0.125, 0.45, 0.15), unseen=area(-0.4, -0.4, 0.4, 0.4))
    assert F.allowed(c, AT, F.spin_poses(AT, left=True)) and F.allowed(c, AT, F.spin_poses(AT, left=False))
    assert 0.3 <= F.highest_cost(c, AT, F.spin_poses(AT, left=True)) < 0.95


def test_a_turn_is_checked_its_own_way() -> None:
    box = cost_with(area(-0.30, -0.30, -0.20, -0.175))  # off the right rear corner, diagonally
    assert F.allowed(box, AT, F.spin_poses(AT, left=True))
    assert not F.allowed(box, AT, F.spin_poses(AT, left=False))


def test_already_near_may_move_away_not_nearer() -> None:  # Review Focus 1
    """A wall 3 cm beyond the left side (its cells' centres 6 cm off): along it is allowed, into it isn't."""
    wall = cost_with(area(-1.0, 0.175, 1.5, 0.3))
    assert F.allowed(wall, AT, [(0.025 * k, 0.0, 0.0) for k in range(1, 9)])  # straight along it
    assert not F.allowed(wall, AT, roll_out(AT, STOP, Command(25.0, 44.0), LIM))  # curving in


def test_highest_cost_ahead_only() -> None:
    wall = cost_with(area(0.60, -1.5, 0.65, 1.5))
    path = [(0.05 * k, 0.0, 0.0) for k in range(1, 11)]  # 50 cm toward a wall 60 cm ahead
    assert F.highest_cost(wall, AT, path, ahead_m=0.1) < F.highest_cost(wall, AT, path)
