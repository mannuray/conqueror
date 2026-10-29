"""Routes on the room map (AI engine LLD §18.6, Plan Z5)."""

from __future__ import annotations

import itertools
import math
import random
import time

import numpy as np
import pytest

from conqueror_ai.domain.drive.clearance import ClearanceMap
from conqueror_ai.domain.drive.route import ROBOT_R_M, RoutePlanner, aim_point
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
