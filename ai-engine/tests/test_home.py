"""Home: what a run keeps for the next, and how it comes back (LLD §20.2-§20.4, design-notes §193)."""

from __future__ import annotations

import numpy as np
import pytest

from conqueror_ai.domain.depth import MapSettings
from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.goals import ExploreGoal, SearchGoal
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.recovery import Recovery
from conqueror_ai.domain.drive.route import RoutePlanner
from conqueror_ai.domain.home import FADE_PER_LOAD, KEEP_PER_WORD, KEEP_RUNS, Home
from conqueror_ai.domain.memory import Remembered, ThingMemory
from conqueror_ai.domain.places import Place
from conqueror_ai.domain.room_map import RoomMap

S = MapSettings()
AT = (0.0, 0.0, 0.0)


def a_run(low: float = 4.0, at: tuple[float, float] = (1.0, 0.0)) -> RoomMap:
    room = RoomMap(S)
    (c,) = room.cells(np.array([at]))
    room.low.reshape(-1)[c] = low
    room.seen[:] = True
    return room


def value_at(room: RoomMap, at: tuple[float, float] = (1.0, 0.0)) -> float:
    (c,) = room.cells(np.array([at]))
    return float(room.low.reshape(-1)[c])


def test_an_obstacle_comes_back_weakened_and_fades() -> None:
    """LLD §20.3 (owner: A): 1.1 -> 0.9 -> 0.7 - loaded twice without being seen again, then gone."""
    home = Home.from_run(a_run(), ThingMemory(), None, "r1")
    room = home.room_for_run(S)
    assert value_at(room) == pytest.approx(
        S.blocked + abs(S.miss), abs=1e-6
    )  # 1.1: weakened, still an obstacle
    home2 = Home.from_run(room, ThingMemory(), home, "r2")  # not seen again
    second = home2.room_for_run(S)
    assert value_at(second) == pytest.approx(S.blocked + abs(S.miss) - FADE_PER_LOAD, abs=1e-6)
    assert value_at(second) > S.blocked
    third = Home.from_run(second, ThingMemory(), home2, "r3").room_for_run(S)
    assert not value_at(third) > S.blocked  # 0.7: gone
    room.miss(room.cells(np.array([(1.0, 0.0)])), "low")
    room.miss(room.cells(np.array([(1.0, 0.0)])), "low")
    assert not value_at(room) > S.blocked  # two looks at free floor clear a loaded one


def test_free_floor_and_weak_evidence_never_come_back() -> None:
    for low in (-2.0, 0.5):
        assert value_at(Home.from_run(a_run(low), ThingMemory(), None, "r").room_for_run(S)) == 0.0


def test_a_loaded_home_never_makes_floor_known_free() -> None:
    """Plan Z8 Review Focus 1 (§191 decision 1): seen comes back only as seen_before; backing goes only over
    floor known free this run."""
    room = Home.from_run(a_run(), ThingMemory(), None, "r").room_for_run(S)
    assert not room.seen.any() and room.seen_before.all()
    cost = CostMap(room.snapshot(0, None, False, True))
    assert not cost.known_free.any()
    lim = Limits()
    assert not Recovery(lim, Planner(lim).foot, lim.drive_tilt_deg)._back_ok(AT, 0.10, cost)  # pyright: ignore[reportPrivateUsage]


def test_explore_and_search_see_a_fresh_map() -> None:
    loaded = Home.from_run(a_run(), ThingMemory(), None, "r").room_for_run(S)
    fresh = RoomMap(S)
    fresh.low[:] = loaded.low
    fresh.high[:] = loaded.high
    for goal in (ExploreGoal, SearchGoal):
        got = [
            goal().destination(0.0, AT, CostMap(r.snapshot(0, None, False, True)), RoutePlanner())
            for r in (loaded, fresh)
        ]
        assert got[0] == got[1]


def test_memory_is_pruned() -> None:
    """Plan Z8 Review Focus 3: the home never grows without bound."""
    mem = ThingMemory(
        [Remembered("ball", float(i), 0.0, float(i), "r1") for i in range(7)]
        + [Remembered("box", 0.0, 1.0, 1.0, "r1", gone=True), Remembered("toy", 0.0, 2.0, 1.0, "old")]
    )
    home = Home.from_run(RoomMap(S), mem, Home.from_run(RoomMap(S), ThingMemory(), None, "r0"), "r1")
    words = [t.word for t in home.things]
    assert words.count("ball") == KEEP_PER_WORD and "box" not in words and "toy" not in words
    assert sorted(t.x_m for t in home.things) == [2.0, 3.0, 4.0, 5.0, 6.0]  # the most recent balls
    h = None
    for k in range(20):
        h = Home.from_run(RoomMap(S), ThingMemory(), h, f"r{k}")
    assert h is not None and len(h.runs) == KEEP_RUNS and h.runs[-1] == "r19"
    assert home.memory().of_kind(lambda w: w == "ball")[0].x_m == 6.0


def test_from_run_keeps_live_places() -> None:
    """LLD §23.3: the names the pages gave are kept with the home."""
    bed = Place(1, "bed", 1.0, 0.0)
    home = Home.from_run(a_run(), ThingMemory(), None, "r1", places=[bed])
    assert home.places == (bed,)


def test_from_run_carries_earlier_places_when_none() -> None:
    """A plain cq-drive --home run has no names of its own: it never wipes the earlier ones."""
    bed = Place(1, "bed", 1.0, 0.0)
    earlier = Home.from_run(a_run(), ThingMemory(), None, "r1", places=[bed])
    assert Home.from_run(a_run(), ThingMemory(), earlier, "r2").places == (bed,)
    assert Home.from_run(a_run(), ThingMemory(), None, "r3").places == ()
    assert Home.from_run(a_run(), ThingMemory(), earlier, "r4", places=[]).places == ()  # all taken off
