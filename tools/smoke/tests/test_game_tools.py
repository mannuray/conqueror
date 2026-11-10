"""Plan G1 Task 6: the pure parts of fake_person.py and game_cap.py."""

from __future__ import annotations

import pytest

from fake_person import Script, distance_at, person_message
from game_cap import pick_cap, speed_cms


def test_the_script_approaches_then_steps_back() -> None:
    s = Script(80.0, 25.0, 3.0, back_cm=60.0, back_after_s=5.0)
    assert distance_at(s, 0.0) == 80.0
    assert distance_at(s, 1.5) == pytest.approx(52.5)
    assert distance_at(s, 4.0) == 25.0
    assert distance_at(s, 5.0) == 60.0


def test_a_person_message_says_coming_or_going() -> None:
    s = Script(80.0, 25.0, 3.0)
    first = person_message(s, 0.0, None)
    assert first == {
        "t": "person",
        "seen": True,
        "d_cm": 80,
        "bearing": 0,
        "coming": 0,
        "sure": 0.9,
        "age_ms": 0,
    }
    assert person_message(s, 1.0, 80.0)["coming"] == 1
    assert person_message(Script(25.0, 80.0, 3.0), 1.0, 25.0)["coming"] == -1
    assert person_message(Script(unseen=True), 0.0, None)["seen"] is False


def test_speed_from_the_encoders() -> None:
    assert speed_cms(1503, 1.0, 7514) == pytest.approx(20.0, abs=0.01)
    with pytest.raises(ValueError):
        speed_cms(100, 0.0, 7514)


def test_the_cap_is_the_largest_step_not_over_25() -> None:
    table = [(50, 15.0), (60, 21.0), (70, 24.9), (80, 27.5), (90, 31.0)]
    assert pick_cap(table) == 70
    assert pick_cap([(50, 26.0)]) is None
