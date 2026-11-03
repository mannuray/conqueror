"""PersonGuard: never a box over a person (AI engine LLD §21.4)."""

from __future__ import annotations

from conqueror_ai.domain.person_guard import clear
from conqueror_ai.domain.types import Detection

PERSON = Detection("person", 0.9, (100.0, 0.0, 200.0, 200.0))


def test_toy_inside_person_dropped() -> None:
    toy = Detection("toy", 0.8, (120.0, 100.0, 160.0, 140.0))
    assert clear([PERSON, toy]) == [PERSON]


def test_toy_beside_person_kept() -> None:
    toy = Detection("toy", 0.8, (300.0, 100.0, 340.0, 140.0))
    edge = Detection("box", 0.8, (190.0, 100.0, 250.0, 160.0))  # 10 of 60 wide overlaps: 0.17 of its own area
    assert clear([PERSON, toy, edge]) == [PERSON, toy, edge]


def test_person_kept() -> None:
    other = Detection("person", 0.7, (110.0, 10.0, 190.0, 190.0))
    assert clear([PERSON, other]) == [PERSON, other]
    assert clear([]) == []


def test_overlap_of_exactly_the_limit_is_dropped() -> None:
    person = Detection("person", 0.9, (0.0, 0.0, 100.0, 100.0))
    box = Detection("box", 0.9, (70.0, 0.0, 170.0, 100.0))  # 30 of its 100 px wide: exactly 0.3 of its area
    assert clear([person, box]) == [person]
    just_under = Detection("box", 0.9, (71.0, 0.0, 171.0, 100.0))
    assert clear([person, just_under]) == [person, just_under]
