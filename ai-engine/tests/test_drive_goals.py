"""Where wander and explore go (AI engine LLD §18.7, Plan Z5)."""

from __future__ import annotations

import math
import random

import numpy as np

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.goals import ExploreGoal, WanderGoal
from conqueror_ai.domain.drive.planner import bearing_deg
from conqueror_ai.domain.drive.route import RoutePlanner
from conqueror_ai.domain.room_map import RoomMap

START = (0.0, 0.0, 0.0)


def area(x0: float, y0: float, x1: float, y1: float) -> list[tuple[float, float]]:
    return [
        (float(x), float(y)) for x in np.arange(x0, x1 + 1e-9, 0.025) for y in np.arange(y0, y1 + 1e-9, 0.025)
    ]


def clear_of(
    seen: list[tuple[float, float]] | None = None,
    blocked: list[tuple[float, float]] | None = None,
    all_seen: bool = False,
    unseen: list[tuple[float, float]] | None = None,
) -> CostMap:
    room = RoomMap()
    if all_seen:
        room.seen[:] = True
    if unseen:
        room.seen.reshape(-1)[room.cells(np.array(unseen, dtype=np.float64))] = False
    if seen:
        room.seen.reshape(-1)[room.cells(np.array(seen, dtype=np.float64))] = True
    if blocked:
        c = room.cells(np.array(blocked, dtype=np.float64))
        room.hit(c, "low")
        room.hit(c, "low")
        room.seen.reshape(-1)[c] = True
    return CostMap(room.snapshot(0, None, False, True))


def test_wander_picks_open_floor_ahead() -> None:
    clear = clear_of(all_seen=True)
    bearings: list[float] = []
    for seed in range(50):
        d = WanderGoal(None, random.Random(seed)).destination(0.0, START, clear)
        assert d is not None
        assert 1.5 - 1e-9 <= math.hypot(*d) <= 2.0 + 1e-9
        assert clear.dist_m(*d) >= 0.30
        bearings.append(abs(bearing_deg(START, d)))
    assert float(np.mean(bearings)) < 60.0


def test_wander_needs_seen_open_floor() -> None:
    assert WanderGoal(None, random.Random(1)).destination(0.0, START, clear_of()) is None  # nothing seen


def test_wander_new_when_near_or_old_or_dropped() -> None:
    clear = clear_of(all_seen=True)
    g = WanderGoal(None, random.Random(2))
    d1 = g.destination(0.0, START, clear)
    assert d1 is not None
    assert g.destination(1.0, START, clear) == d1  # kept
    near = (d1[0] - 0.3, d1[1], 0.0)
    assert g.destination(1.1, near, clear) != d1  # reached: a new one
    d2 = g.destination(1.2, START, clear)
    assert g.destination(7.3, START, clear) != d2  # 6 s old
    d3 = g.destination(7.4, START, clear)
    g.drop()
    assert g.destination(7.5, START, clear) != d3


def test_wander_done_after_seconds() -> None:
    clear = clear_of(all_seen=True)
    g = WanderGoal(60.0, random.Random(3))
    g.destination(10.0, START, clear)
    assert not g.done(69.9) and g.done(70.0)


def test_wander_without_seconds_never_done() -> None:
    g = WanderGoal(None, random.Random(4))
    g.destination(0.0, START, clear_of(all_seen=True))
    assert not g.done(1e6)


def test_explore_goes_to_the_nearest_frontier() -> None:
    clear = clear_of(seen=area(-0.5, -1.0, 1.5, 1.0))  # seen in front up to 1.5 m, unseen beyond
    d = ExploreGoal().destination(0.0, START, clear, RoutePlanner())
    assert d is not None
    assert abs(d[0] - 1.45) <= 0.15 or abs(abs(d[1]) - 0.95) <= 0.15 or abs(d[0] + 0.45) <= 0.15


def test_explore_ignores_tiny_unseen_bits() -> None:
    hole = clear_of(all_seen=True, unseen=area(0.95, -0.1, 1.15, 0.1))
    g = ExploreGoal()
    assert g.destination(0.0, START, hole, RoutePlanner()) is None  # a 20 cm hole is no frontier
    assert g.done


def test_explore_done_when_nothing_left() -> None:  # Review Focus 5
    g = ExploreGoal()
    assert g.destination(0.0, START, clear_of(all_seen=True), RoutePlanner()) is None
    assert g.done


def test_explore_done_when_unreachable() -> None:
    walls = area(-1.0, -1.0, 1.0, -0.9) + area(-1.0, 0.9, 1.0, 1.0) + area(-1.0, -1.0, -0.9, 1.0)
    walls += area(0.9, -1.0, 1.0, 1.0)
    g = ExploreGoal()
    assert (
        g.destination(0.0, START, clear_of(seen=area(-0.9, -0.9, 0.9, 0.9), blocked=walls), RoutePlanner())
        is None
    )
    assert g.done


def test_explore_does_not_pick_a_reached_frontier_again() -> None:
    clear = clear_of(seen=area(-0.5, -1.0, 1.5, 1.0))
    g = ExploreGoal()
    d = g.destination(0.0, START, clear, RoutePlanner())
    assert d is not None
    d2 = g.destination(0.5, (d[0], d[1], 0.0), clear, RoutePlanner())  # arrived
    assert d2 is None or math.dist(d2, d) > 0.25


def test_explore_picks_quickly_by_searching_outward() -> None:  # review M1 (Plan Z6)
    """The cheapest frontier is the first one an outward search reaches: no whole-map search every 3 s
    (it took ~12 ms here and pushed ticks to ~30 ms)."""
    import time

    c = clear_of(seen=area(-3.9, -3.9, 3.9, 1.0))  # unseen beyond y = 1 m
    g, rp = ExploreGoal(), RoutePlanner()
    t = time.perf_counter()
    d = g.destination(0.0, START, c, rp)
    assert d is not None and abs(d[1] - 1.0) < 0.15
    assert (time.perf_counter() - t) * 1000 < 5.0
