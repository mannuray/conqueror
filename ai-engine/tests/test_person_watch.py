"""PersonWatch (AI engine LLD §28.2, Plan G2): where the person is, from the front edge, pessimistic."""

from __future__ import annotations

import pytest

from conqueror_ai.domain.person import PersonWatch
from tests.test_person_test import box_at, cam_down

S = 1_000_000  # µs per second


def watch_at(d: float, bearing: float = 0.0, t_us: int = 0, sonar: float | None = None) -> PersonWatch:
    w = PersonWatch()
    cam = cam_down()
    w.update([box_at(cam, d, bearing)], cam, sonar, t_us)
    return w


def test_one_metre_ahead_reads_one_metre_less_two_sigma() -> None:
    e = watch_at(1.0).latest(0)
    assert e is not None and e.seen
    assert e.d_m == pytest.approx(0.80, abs=0.02)  # sigma 0.10 m up to 1 m
    assert e.bearing_deg == pytest.approx(0.0, abs=1.0)


def test_beyond_one_metre_the_margin_grows() -> None:
    e = watch_at(1.5).latest(0)
    assert e is not None and e.d_m == pytest.approx(1.5 - 0.6, abs=0.03)  # sigma 0.20 x 1.5


def test_the_sonar_wins_near_and_ahead() -> None:
    e = watch_at(0.9, 0.0, sonar=0.6).latest(0)
    assert e is not None and e.d_m == pytest.approx(0.6 - 0.2, abs=0.02)
    aside = watch_at(0.9, 30.0, sonar=0.6).latest(0)  # outside ±12°: not the person's echo
    assert aside is not None and aside.d_m == pytest.approx(0.9 - 0.2, abs=0.03)


def test_a_box_cut_by_the_pictures_bottom_reads_close() -> None:
    w = PersonWatch()
    cam = cam_down()
    w.update([(100.0, 100.0, 200.0, 239.0)], cam, None, 0)  # the test camera is 320 x 240
    e = w.latest(0)
    assert e is not None and e.seen and e.d_m is not None and e.d_m <= 0.35


def test_of_two_people_the_nearer_counts() -> None:
    w = PersonWatch()
    cam = cam_down()
    w.update([box_at(cam, 1.6, 0.0), box_at(cam, 0.9, 20.0)], cam, None, 0)
    e = w.latest(0)
    assert e is not None and e.bearing_deg == pytest.approx(20.0, abs=1.5)


def test_coming_and_going() -> None:
    cam = cam_down()
    w = PersonWatch()
    for i in range(7):  # 1.0 -> 0.7 m over 0.6 s
        w.update([box_at(cam, 1.0 - 0.05 * i, 0.0)], cam, None, i * 100_000)
    e = w.latest(600_000)
    assert e is not None and e.coming == 1
    w2 = PersonWatch()
    for i in range(7):
        w2.update([box_at(cam, 0.7 + 0.05 * i, 0.0)], cam, None, i * 100_000)
    e2 = w2.latest(600_000)
    assert e2 is not None and e2.coming == -1
    w3 = PersonWatch()
    for i in range(7):
        w3.update([box_at(cam, 1.0 + 0.005 * i, 0.0)], cam, None, i * 100_000)
    e3 = w3.latest(600_000)
    assert e3 is not None and e3.coming == 0


def test_last_seen_close_counts_down_to_caught() -> None:
    cam = cam_down()
    w = PersonWatch()
    for i in range(6):  # 0.70 -> 0.60 m, coming at ~0.2 m/s; reported 0.40 m at the end
        w.update([box_at(cam, 0.70 - 0.02 * i, 0.0)], cam, None, i * 100_000)
    last = w.latest(500_000)
    assert last is not None and last.d_m is not None and last.d_m < 0.5
    w.update([], cam, None, 600_000)  # gone from the picture
    readings: list[float] = []
    for t in (600_000, 900_000, 1_200_000, 1_500_000):
        w.update([], cam, None, t)
        e = w.latest(t)
        assert e is not None and not e.seen and e.d_m is not None
        readings.append(e.d_m)
    assert readings == sorted(readings, reverse=True)  # never rising
    assert readings[-1] <= 0.30 + 1e-9  # 1 s unseen: 30 cm (caught)


def test_last_seen_far_does_not_count_down() -> None:
    cam = cam_down()
    w = PersonWatch()
    w.update([box_at(cam, 1.6, 0.0)], cam, None, 0)
    w.update([], cam, None, 2 * S)
    e = w.latest(2 * S)
    assert e is not None and not e.seen and e.d_m is not None and e.d_m > 0.5


def test_the_wire_message_ages_with_the_picture() -> None:
    w = watch_at(0.9, 10.0, t_us=1 * S)  # not on the 1 m boundary (10 cm / 20 % margins)
    e = w.latest(1 * S)
    assert e is not None
    m = e.wire(1 * S + 250_000)  # the newest picture is 250 ms old (a frozen camera's grows)
    assert m["t"] == "person" and m["seen"] is True
    assert m["d_cm"] == 70 and m["bearing"] == 10 and m["age_ms"] == 250
    assert m["coming"] in (-1, 0, 1) and 0.0 <= m["sure"] <= 1.0
    assert PersonWatch().latest(0) is None
