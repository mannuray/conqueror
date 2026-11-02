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
    mem.update([seen(1, "toy", 1.0, 0.5, 3.0)])
    mem.update([])  # the tracker forgot it
    (r,) = mem.of_kind(toy)
    assert (r.word, r.x_m, r.y_m, r.last_seen_s) == ("toy", 1.0, 0.5, 3.0)


def test_a_thing_seen_again_nearby_is_the_same_one_moved() -> None:
    mem = ThingMemory()
    mem.update([seen(1, "toy", 1.0, 0.0, 1.0)])
    mem.update([seen(7, "toy", 1.0 + MERGE_M - 0.05, 0.0, 5.0)])  # a new tracker id, the same toy
    (r,) = mem.of_kind(toy)
    assert r.x_m == 1.0 + MERGE_M - 0.05 and r.last_seen_s == 5.0
    mem.update([seen(8, "toy", 3.0, 0.0, 6.0)])  # another one
    assert len(mem.of_kind(toy)) == 2


def test_only_things_seen_now_and_never_a_person() -> None:
    mem = ThingMemory()
    mem.update([seen(1, "person", 1.0, 0.0, 1.0), seen(2, "box", 2.0, 0.0, 1.0, state="remembered")])
    assert mem.of_kind(lambda c: True) == []


def test_most_recent_first_and_gone_ones_left_out_until_seen_again() -> None:
    mem = ThingMemory()
    mem.update([seen(1, "toy", 1.0, 0.0, 1.0), seen(2, "toy", 3.0, 0.0, 4.0), seen(3, "box", 2.0, 2.0, 5.0)])
    old, new = sorted(mem.of_kind(toy), key=lambda r: r.last_seen_s)
    assert mem.of_kind(toy) == [new, old]
    mem.mark_gone(new)
    assert mem.of_kind(toy) == [old]
    mem.update([seen(9, "toy", 3.1, 0.0, 9.0)])  # back where it was
    assert [r.x_m for r in mem.of_kind(toy)] == [3.1, 1.0]
