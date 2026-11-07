"""Routes on the room map (AI engine LLD §18.6, Plan Z5)."""

from __future__ import annotations

import itertools
import math
import random
import time

import numpy as np
import pytest

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.route import XY, RoutePlanner, aim_point
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from tests.route_reference import ReferencePlanner

HALF_W = Limits().body_half_w_m  # a route keeps the tank's centre this far from known obstacles (Plan Z6)


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
    r = RoutePlanner().route(CostMap(snap([])), (0.0, 0.0), (2.0, 0.0))
    assert r is not None and r[0] == (0.0, 0.0) and r[-1] == (2.0, 0.0)
    assert length(r) <= 2.1


def test_around_a_box() -> None:
    c = CostMap(snap(box(0.8, -0.2, 1.2, 0.2)))
    r = RoutePlanner().route(c, (0.0, 0.0), (2.0, 0.0))
    assert r is not None
    assert all(c.dist_m(x, y) >= HALF_W for x, y in r[1:-1])


def test_out_of_a_u() -> None:
    u = box(0.4, -0.6, 0.5, 0.6) + box(-0.6, 0.5, 0.5, 0.6) + box(-0.6, -0.6, 0.5, -0.5)  # open toward -x
    r = RoutePlanner().route(CostMap(snap(u)), (0.0, 0.0), (2.0, 0.0))
    assert r is not None and min(x for x, _ in r) < -0.5


def test_no_route() -> None:
    walls = (
        box(1.5, -0.6, 1.6, 0.6)
        + box(2.4, -0.6, 2.5, 0.6)
        + box(1.5, 0.5, 2.5, 0.6)
        + box(1.5, -0.6, 2.5, -0.5)
    )
    assert RoutePlanner().route(CostMap(snap(walls)), (0.0, 0.0), (2.0, 0.0)) is None


def test_unseen_costs_more() -> None:
    wall = box(0.9, -0.1, 1.1, 0.1)  # go round above (seen) or below (unseen)
    c = CostMap(snap(wall, unseen=box(0.5, -1.0, 1.5, -0.1)))
    r = RoutePlanner().route(c, (0.0, 0.0), (2.0, 0.0))
    assert r is not None and all(y > -0.05 for _, y in r)


def test_start_cell_allowed() -> None:
    c = CostMap(
        snap(box(-0.2, 0.125, 0.2, 0.3))
    )  # just beyond the left side (Plan Z6: the body clears under it)
    assert c.dist_m(0.0, 0.0) < HALF_W
    assert RoutePlanner().route(c, (0.0, 0.0), (1.5, -0.5)) is not None


def test_target_in_its_own_marks() -> None:  # Review Focus 2
    ball = box(1.45, -0.05, 1.55, 0.05)
    assert RoutePlanner().route(CostMap(snap(ball)), (0.0, 0.0), (1.5, 0.0)) is None
    c = CostMap(snap(ball), ignore=(1.5, 0.0, 0.12))
    assert RoutePlanner().route(c, (0.0, 0.0), (1.5, 0.0), goal_ok=True) is not None


def test_goal_outside_map_is_none() -> None:  # Review Focus 3
    c = CostMap(snap([]))
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
    costs = p.costs(CostMap(snap(walls)), (0.0, 0.0))
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
    c = CostMap(snap(pts))
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
    clear = CostMap(snap(box(1.56, -0.8, 2.0, 0.8)), ignore=(1.5, 0.0, 0.12))
    p = RoutePlanner()
    assert p.route(clear, (0.0, 0.0), (1.5, 0.0), goal_ok=True) is None
    r = p.route(clear, (0.0, 0.0), (1.5, 0.0), goal_ok=True, near_m=0.485)
    assert r is not None and r[-1] == (1.5, 0.0)
    assert math.dist(r[-2], (1.5, 0.0)) <= 0.485 + 0.08
    assert all(clear.dist_m(x, y) >= HALF_W for x, y in r[1:-1])


# ---- routes keep room to turn round (design-notes §165)
def corridor(half_gap: float) -> MapSnapshot:
    """A wall across the room at x 0.5-2.0 m with one corridor through it, its walls half_gap from the
    middle (cell centres)."""
    return snap(box(0.5, half_gap, 2.0, 3.9) + box(0.5, -3.9, 2.0, -half_gap))


def test_doorways_stay_open() -> None:
    """A 46 cm doorway (Plan Z5's simulated one, cell-aligned the same) has a route (Plan Z6: closed only
    where the body would overlap a known obstacle going straight)."""
    lo, hi = 0.8 - 0.23, 0.8 + 0.23
    walls = box(1.0, -2.5, 1.1, lo) + box(1.0, hi, 1.1, 2.5)
    assert RoutePlanner().route(CostMap(snap(walls)), (0.0, 0.0), (2.0, 0.8)) is not None


def test_room_to_turn_comes_from_cost() -> None:
    """Floor run 2 drove 15 cm beside the bed into a spot it could only back out of: a route would rather keep
    room to turn round - from the near cost, no rule (LLD §18.13.3). A ~40 cm gap straight ahead and a ~60 cm
    one 80 cm to the left: through the wide one; the narrow alone: still a way."""
    narrow = CostMap(corridor(0.225))
    assert RoutePlanner().route(narrow, (0.0, 0.0), (2.5, 0.0)) is not None
    both = snap(box(0.5, -3.9, 2.0, -0.225) + box(0.5, 0.225, 2.0, 0.475) + box(0.5, 1.125, 2.0, 3.9))
    via = RoutePlanner().route(CostMap(both), (0.0, 0.0), (2.5, 0.0))
    assert via is not None and max(y for _, y in via) > 0.5


def test_routes_are_closed_by_the_bodys_own_width() -> None:
    """A cell is closed only where the body would overlap a known obstacle going straight: the half-width
    from Limits (drive.yaml may widen it). Walls at +-22.5 cm (cell centres): open for the tank, closed
    for a body 32 cm wide."""
    gap = CostMap(corridor(0.225))
    assert RoutePlanner(Limits()).route(gap, (0.0, 0.0), (2.5, 0.0)) is not None
    assert RoutePlanner(Limits(body_half_w_m=0.16)).route(gap, (0.0, 0.0), (2.5, 0.0)) is None


def _random_map(rng: random.Random) -> tuple[CostMap, list[tuple[float, float]]]:
    """A room of boxes and a wall or two, some floor unseen; returns the cost map and its box corners."""
    pts: list[tuple[float, float]] = []
    corners: list[tuple[float, float]] = []
    for _ in range(rng.randint(5, 40)):
        x, y = rng.uniform(-3.5, 3.0), rng.uniform(-3.5, 3.0)
        pts += box(x, y, x + rng.uniform(0.1, 0.5), y + rng.uniform(0.1, 0.5))
        corners.append((x, y))
    for _ in range(rng.randint(0, 2)):
        x = rng.uniform(-3.0, 3.0)
        pts += box(x, -3.9, x + 0.05, rng.uniform(-1.0, 3.0))
    ux, uy = rng.uniform(-3.0, 2.0), rng.uniform(-3.0, 2.0)
    return CostMap(snap(pts, box(ux, uy, ux + 1.0, uy + 1.0))), corners


def _queries(rng: random.Random, corners: list[tuple[float, float]]) -> list[tuple[XY, XY, bool, float]]:
    def anywhere() -> XY:
        return (rng.uniform(-3.9, 3.9), rng.uniform(-3.9, 3.9))

    qs = [(anywhere(), anywhere(), rng.random() < 0.3, rng.choice([0.0, 0.0, 0.4])) for _ in range(20)]
    qs.append((corners[0], anywhere(), False, 0.0))  # the start inside a mark
    qs.append((anywhere(), corners[-1], True, 0.0))  # the goal on a mark, allowed
    qs.append((anywhere(), corners[-1], False, 0.0))  # the goal on a mark, not allowed
    qs.append((anywhere(), (50.0, 0.0), False, 0.0))  # an end off the map
    return qs


def test_the_quick_planner_gives_todays_routes() -> None:
    """Plan Z12 (LLD §27.4): exactly today's routes, nearest cells and costs, not just equally cheap ones."""
    rng = random.Random(12)
    for _ in range(30):
        c, corners = _random_map(rng)
        new, ref = RoutePlanner(), ReferencePlanner()
        for a, b, ok, near in _queries(rng, corners):
            assert new.route(c, a, b, ok, near) == ref.route(c, a, b, ok, near)
        a = (rng.uniform(-3.9, 3.9), rng.uniform(-3.9, 3.9))
        for every in (97, 13):

            def accept(i: int, j: int, every: int = every) -> bool:
                return (i * 31 + j * 17) % every == 0

            assert new.nearest(c, a, accept) == ref.nearest(c, a, accept)
        assert np.array_equal(new.costs(c, a), ref.costs(c, a))
        assert np.array_equal(new.costs(c, corners[0]), ref.costs(c, corners[0]))


def test_the_quick_planner_is_about_twice_as_quick() -> None:
    """Plan Z12 (LLD §27.4): ~2.1-2.2x measured; at least 1.8x (owner, 2026-11-08), a ratio on one machine."""
    rng = random.Random(21)
    work: list[tuple[CostMap, list[tuple[XY, XY, bool, float]]]] = []
    for _ in range(10):
        c, corners = _random_map(rng)
        work.append((c, _queries(rng, corners)))

    def run(p: RoutePlanner | ReferencePlanner) -> float:
        t = time.perf_counter()
        for c, qs in work:
            for a, b, ok, near in qs:
                p.route(c, a, b, ok, near)
            p.costs(c, qs[0][0])
        return time.perf_counter() - t

    new, ref = RoutePlanner(), ReferencePlanner()
    for p in (new, ref):  # build each grid once (cached per cost map) before timing
        run(p)
    t_new, t_ref = math.inf, math.inf
    for _ in range(5):  # taken in turns, the best of each: a busy spell hits both alike
        t_new, t_ref = min(t_new, run(new)), min(t_ref, run(ref))
    print(f"quick {t_new * 1000:.0f} ms, today's {t_ref * 1000:.0f} ms, {t_ref / t_new:.2f}x")
    assert t_ref / t_new >= 1.8
