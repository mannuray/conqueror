"""ThingMemory: what the tank confirmed, and where (design-notes §189, AI engine LLD §18.13.23)."""

from __future__ import annotations

from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.reach import SAME_SPOT_M as MERGE_M
from conqueror_ai.domain.types import ObjectEstimate


def seen(oid: int, cls: str, x: float, y: float, t_s: float, state: str = "seen") -> ObjectEstimate:
    us = round(t_s * 1e6)
    return ObjectEstimate(oid, cls, x, y, 0.0, 0.0, 0.1, us, us, state)


def toy(cls: str) -> bool:
    return cls == "toy"


def test_a_thing_is_kept_long_after_the_tracker_forgets_it() -> None:
    """The tracker forgets a thing 20 s out of view (FORGET_S); the memory keeps it for the run."""
    mem = ThingMemory()
    mem.update([seen(1, "toy", 1.0, 0.5, 3.0)], 3.0, "r")
    mem.update([], 30.0, "r")  # the tracker forgot it
    (r,) = mem.of_kind(toy)
    assert (r.word, r.x_m, r.y_m, r.seen_wall_s) == ("toy", 1.0, 0.5, 3.0)


def test_a_thing_seen_again_nearby_is_the_same_one_moved() -> None:
    mem = ThingMemory()
    mem.update([seen(1, "toy", 1.0, 0.0, 1.0)], 1.0, "r")
    mem.update([seen(7, "toy", 1.0 + MERGE_M - 0.05, 0.0, 5.0)], 5.0, "r")  # a new tracker id, the same toy
    (r,) = mem.of_kind(toy)
    assert r.x_m == 1.0 + MERGE_M - 0.05 and r.seen_wall_s == 5.0
    mem.update([seen(8, "toy", 3.0, 0.0, 6.0)], 6.0, "r")  # another one
    assert len(mem.of_kind(toy)) == 2


def test_only_things_seen_now_and_never_a_person() -> None:
    mem = ThingMemory()
    mem.update(
        [seen(1, "person", 1.0, 0.0, 1.0), seen(2, "box", 2.0, 0.0, 1.0, state="remembered")], 1.0, "r"
    )
    assert mem.of_kind(lambda c: True) == []


def test_most_recent_first_and_gone_ones_left_out_until_seen_again() -> None:
    mem = ThingMemory()
    mem.update([seen(1, "toy", 1.0, 0.0, 1.0)], 1.0, "r")
    mem.update([seen(2, "toy", 3.0, 0.0, 4.0), seen(3, "box", 2.0, 2.0, 5.0)], 4.0, "r")
    old, new = sorted(mem.of_kind(toy), key=lambda r: r.seen_wall_s)
    assert mem.of_kind(toy) == [new, old]
    mem.mark_gone(new)
    assert mem.of_kind(toy) == [old]
    mem.update([seen(9, "toy", 3.1, 0.0, 9.0)], 9.0, "r")  # back where it was
    assert [r.x_m for r in mem.of_kind(toy)] == [3.1, 1.0]


def test_memory_orders_by_wall_time_across_runs() -> None:
    """Plan Z7 (LLD §19.7, review F7.2): the bridge's capture clock restarts at every power-on, so the order
    of things from different runs comes from the wall clock and the run they were seen in."""
    mem = ThingMemory()
    mem.update([seen(1, "toy", 1.0, 0.0, 500.0)], wall_s=1_000.0, run_id="run-1")  # capture clock 500 s
    mem.update([seen(2, "toy", 3.0, 0.0, 2.0)], wall_s=9_000.0, run_id="run-2")  # restarted: 2 s
    first, second = mem.of_kind(toy)
    assert (first.x_m, first.run_id, first.seen_wall_s) == (3.0, "run-2", 9_000.0)
    assert (second.x_m, second.run_id) == (1.0, "run-1")
