"""Candidates: sightings of the thing sought, checked before unlooked floor (design-notes §189)."""

from __future__ import annotations

import numpy as np

from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.drive.candidates import Candidates
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.drive.route import RoutePlanner
from conqueror_ai.domain.memory import Remembered
from conqueror_ai.domain.reach import SAME_SPOT_M as MERGE_M
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.types import Pose

AT = (0.0, 0.0, 0.0)


def room(marks: list[tuple[float, float]] = ()) -> MapSnapshot:  # type: ignore[assignment]
    r = RoomMap()
    r.seen[:] = True
    z = np.zeros((0, 2))
    r.update_picture(Marks(z, z, z, z), 0)
    if marks:
        c = r.cells(np.array(marks, dtype=np.float64))
        r.hit(c, "low")
        r.hit(c, "low")
    return r.snapshot(0, Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), False, True)


def test_sightings_within_half_a_metre_are_one_candidate_at_the_newest_spot() -> None:
    c = Candidates()
    c.add((1.0, 0.0))
    c.add((1.0 + MERGE_M - 0.05, 0.1))
    c.add((2.5, 0.0))
    assert [(round(k.x_m, 2), round(k.y_m, 2)) for k in c.pending()] == [(1.45, 0.1), (2.5, 0.0)]


def test_a_ruled_out_spot_ignores_later_sightings() -> None:
    c = Candidates()
    c.add((1.0, 0.0))
    (k,) = c.pending()
    c.rule_out(k)
    c.add((1.2, 0.0))
    assert c.pending() == []


def test_next_is_the_cheapest_to_reach() -> None:
    c = Candidates()
    c.add((2.0, 0.0))
    c.add((-1.0, 0.0))
    k = c.next(AT, CostMaps(room(), None, None), RoutePlanner())
    assert k is not None and (k.x_m, k.y_m) == (-1.0, 0.0)


def test_a_candidates_own_marks_do_not_count_but_outside_the_area_never() -> None:
    """b1 (§189): the thing on the floor marks its own cells (depth sees it as low); within 12 cm they are set
    aside, as the chase does. Outside the play area a candidate is left alone."""
    c = Candidates()
    c.add((1.5, 0.0))
    snap = room([(1.5 + dx, dy) for dx in (-0.05, 0.0, 0.05) for dy in (-0.05, 0.0, 0.05)])
    assert c.next(AT, CostMaps(snap, None, None), RoutePlanner()) is not None
    assert c.next(AT, CostMaps(snap, None, (0.0, 0.0, 1.0)), RoutePlanner()) is None


def test_remembered_spots_are_candidates_that_know_where_they_came_from() -> None:
    c = Candidates()
    r = Remembered("toy", 1.0, 1.0, 3.0, "r")
    c.seed([r])
    (k,) = c.pending()
    assert k.remembered is r and (k.x_m, k.y_m) == (1.0, 1.0)


def test_twenty_pending_spots_cost_at_most_three_routes_a_try() -> None:
    """Review F6 / Plan Z7 Review Focus 2: one route search per pending candidate per try (6-21 ms each) -
    with 20 remembered spots a tick would take ~0.4 s, past the drive's 0.5 s freshness. Nearest first, at
    most ROUTE_TRIES routes."""
    from conqueror_ai.domain.drive.candidates import ROUTE_TRIES

    class Counting(RoutePlanner):
        calls = 0

        def route(self, *a: object, **k: object) -> list[tuple[float, float]] | None:  # type: ignore[override]
            Counting.calls += 1
            return super().route(*a, **k)  # type: ignore[arg-type]

    c = Candidates()
    for i in range(20):
        c.add((0.6 * (i + 1), 0.0) if i % 2 else (0.0, 0.6 * (i + 1)))
    k = c.next(AT, CostMaps(room(), None, None), Counting())
    assert ROUTE_TRIES == 3 and Counting.calls <= 3
    assert k is not None and (k.x_m, k.y_m) == (0.0, 0.6)
