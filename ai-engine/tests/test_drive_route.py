"""Routes on the room map (AI engine LLD §18.6, Plan Z5)."""

from __future__ import annotations

import itertools
import math
import random
import time

import numpy as np
import pytest

from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.route import ROBOT_R_M, RoutePlanner, aim_point, turn_room_m
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap


def box(x0: float, y0: float, x1: float, y1: float) -> list[tuple[float, float]]:
    xs = np.arange(x0, x1 + 1e-9, 0.025)
    ys = np.arange(y0, y1 + 1e-9, 0.025)
    return [(float(x), float(y)) for x in xs for y in ys]


def snap(blocked: list[tuple[float, float]], unseen: list[tuple[float, float]] | None = None) -> MapSnapshot:
    room = RoomMap()
    if blocked:
        c = room.cells(np.array(blocked, dtype=np.float64))
        room.hit(c, "low")
        room.hit(c, "low")
    room.seen[:] = True
    if unseen:
        room.seen.reshape(-1)[room.cells(np.array(unseen, dtype=np.float64))] = False
    return room.snapshot(0, None, False, True)


def length(route: list[tuple[float, float]]) -> float:
    return sum(math.dist(a, b) for a, b in itertools.pairwise(route))


def test_straight_route_in_the_open() -> None:
    r = RoutePlanner().route(ClearanceMap(snap([])), (0.0, 0.0), (2.0, 0.0))
    assert r is not None and r[0] == (0.0, 0.0) and r[-1] == (2.0, 0.0)
    assert length(r) <= 2.1


def test_around_a_box() -> None:
    c = ClearanceMap(snap(box(0.8, -0.2, 1.2, 0.2)))
    r = RoutePlanner().route(c, (0.0, 0.0), (2.0, 0.0))
    assert r is not None
    assert all(c.dist_m(x, y) >= ROBOT_R_M for x, y in r[1:-1])


def test_out_of_a_u() -> None:
    u = box(0.4, -0.6, 0.5, 0.6) + box(-0.6, 0.5, 0.5, 0.6) + box(-0.6, -0.6, 0.5, -0.5)  # open toward -x
    r = RoutePlanner().route(ClearanceMap(snap(u)), (0.0, 0.0), (2.0, 0.0))
    assert r is not None and min(x for x, _ in r) < -0.5


def test_no_route() -> None:
    walls = (
        box(1.5, -0.6, 1.6, 0.6)
        + box(2.4, -0.6, 2.5, 0.6)
        + box(1.5, 0.5, 2.5, 0.6)
        + box(1.5, -0.6, 2.5, -0.5)
    )
    assert RoutePlanner().route(ClearanceMap(snap(walls)), (0.0, 0.0), (2.0, 0.0)) is None


def test_unseen_costs_more() -> None:
    wall = box(0.9, -0.1, 1.1, 0.1)  # go round above (seen) or below (unseen)
    c = ClearanceMap(snap(wall, unseen=box(0.5, -1.0, 1.5, -0.1)))
    r = RoutePlanner().route(c, (0.0, 0.0), (2.0, 0.0))
    assert r is not None and all(y > -0.05 for _, y in r)


def test_start_cell_allowed() -> None:
    c = ClearanceMap(snap(box(-0.2, 0.08, 0.2, 0.3)))
    assert c.dist_m(0.0, 0.0) < ROBOT_R_M
    assert RoutePlanner().route(c, (0.0, 0.0), (1.5, -0.5)) is not None


def test_target_in_its_own_marks() -> None:  # Review Focus 2
    ball = box(1.45, -0.05, 1.55, 0.05)
    assert RoutePlanner().route(ClearanceMap(snap(ball)), (0.0, 0.0), (1.5, 0.0)) is None
    c = ClearanceMap(snap(ball), ignore=(1.5, 0.0, 0.12))
    assert RoutePlanner().route(c, (0.0, 0.0), (1.5, 0.0), goal_ok=True) is not None


def test_goal_outside_map_is_none() -> None:  # Review Focus 3
    c = ClearanceMap(snap([]))
    assert RoutePlanner().route(c, (0.0, 0.0), (5.0, 0.0)) is None
    assert RoutePlanner().route(c, (9.0, 0.0), (1.0, 0.0)) is None


def test_aim_point() -> None:
    r = [(0.0, 0.0), (0.1, 0.0), (0.2, 0.0), (0.5, 0.0), (1.0, 0.0), (2.0, 0.0)]
    x, y = aim_point(r, (0.0, 0.05))
    assert x == pytest.approx(0.5, abs=0.01) and y == pytest.approx(0.0)
    assert aim_point(r, (1.8, 0.0)) == (2.0, 0.0)
    assert aim_point([(1.0, 1.0)], (0.0, 0.0)) == (1.0, 1.0)


def test_costs_inf_where_walled() -> None:
    walls = (
        box(1.5, -0.6, 1.6, 0.6)
        + box(2.4, -0.6, 2.5, 0.6)
        + box(1.5, 0.5, 2.5, 0.6)
        + box(1.5, -0.6, 2.5, -0.5)
    )
    p = RoutePlanner()
    costs = p.costs(ClearanceMap(snap(walls)), (0.0, 0.0))
    i, j = p.grid_of(2.0, 0.0)
    assert math.isinf(costs[i, j])
    i, j = p.grid_of(1.0, 1.0)
    assert 1.0 < costs[i, j] < 3.0


def test_route_is_quick() -> None:
    rng = random.Random(3)
    pts: list[tuple[float, float]] = []
    for _ in range(30):
        x, y = rng.uniform(-3.5, 3.0), rng.uniform(-3.5, 3.0)
        pts += box(x, y, x + rng.uniform(0.1, 0.5), y + rng.uniform(0.1, 0.5))
    c = ClearanceMap(snap(pts))
    p = RoutePlanner()
    slowest = 0.0
    for _ in range(20):
        a = (rng.uniform(-3.8, 3.8), rng.uniform(-3.8, 3.8))
        b = (rng.uniform(-3.8, 3.8), rng.uniform(-3.8, 3.8))
        t = time.perf_counter()
        p.route(c, a, b)
        slowest = max(slowest, time.perf_counter() - t)
    assert slowest < 0.02


def test_a_goal_against_furniture_is_reached_within_reach() -> None:
    """Review I2: a ball 6 cm from a sofa - every cell next to it is too near the sofa. With near_m the route
    ends at the first open cell within reach of the ball."""
    clear = ClearanceMap(snap(box(1.56, -0.8, 2.0, 0.8)), ignore=(1.5, 0.0, 0.12))
    p = RoutePlanner()
    assert p.route(clear, (0.0, 0.0), (1.5, 0.0), goal_ok=True) is None
    r = p.route(clear, (0.0, 0.0), (1.5, 0.0), goal_ok=True, near_m=0.485)
    assert r is not None and r[-1] == (1.5, 0.0)
    assert math.dist(r[-2], (1.5, 0.0)) <= 0.485 + 0.08
    assert all(clear.dist_m(x, y) >= ROBOT_R_M for x, y in r[1:-1])


# ---- routes keep room to turn round (design-notes §165)
def corridor(half_gap: float) -> MapSnapshot:
    """A wall across the room at x 0.5-2.0 m with one corridor through it, its walls half_gap from the
    middle (cell centres)."""
    return snap(box(0.5, half_gap, 2.0, 3.9) + box(0.5, -3.9, 2.0, -half_gap))


def test_the_turn_room_is_the_bodys_turning_circle_and_2_cm() -> None:
    assert turn_room_m(Limits()) == pytest.approx(math.hypot(0.13, 0.115) + 0.02)
    assert turn_room_m(Limits(body_half_w_m=0.15)) > turn_room_m(Limits())


def test_routes_keep_room_to_turn_round_where_they_can() -> None:
    """Floor run 2 drove 15 cm beside the bed into a spot it could only back out of. A route prefers room to
    turn round (cells nearer than that cost TIGHT_W times more) but still takes a narrow gap when it is the
    only way: a hard limit closed 46-60 cm doorways (the 5 cm cells, design-notes §165)."""
    turn = RoutePlanner(turn_room_m(Limits()))
    narrow = ClearanceMap(corridor(0.225), grow=False)  # a ~40 cm gap, the only way
    assert turn.route(narrow, (0.0, 0.0), (2.5, 0.0)) is not None
    both = snap(box(0.5, -3.9, 2.0, -0.225) + box(0.5, 0.225, 2.0, 0.475) + box(0.5, 1.125, 2.0, 3.9))
    two = ClearanceMap(both, grow=False)  # the ~40 cm gap straight ahead, a ~60 cm one 80 cm to the left
    via = turn.route(two, (0.0, 0.0), (2.5, 0.0))
    assert via is not None and max(y for _, y in via) > 0.5  # room to turn: through the wide one
