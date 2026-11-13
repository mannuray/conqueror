"""PersonWatch (AI engine LLD §28.2, Plan G2): where the person is, from the front edge, pessimistic."""

from __future__ import annotations

import itertools

import pytest

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.person import PersonWatch, person_range
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
    assert e is not None and e.d_m == pytest.approx(1.5 - 0.3, abs=0.03)  # sigma 0.10 x 1.5


def test_farther_never_reads_nearer() -> None:
    """One margin rule, no jump at 1 m (the floor runs: 1.05 m read 0.6, nearer than 1.0 m's 0.8)."""
    reads = [watch_at(d / 100).latest(0) for d in range(80, 160, 5)]
    ds = [e.d_m for e in reads if e is not None and e.d_m is not None]
    assert len(ds) == len(reads)
    assert all(b >= a - 0.01 for a, b in itertools.pairwise(ds))


def test_the_sonar_wins_near_and_ahead() -> None:
    e = watch_at(0.9, 0.0, sonar=0.6).latest(0)
    assert e is not None and e.d_m == pytest.approx(0.6 - 0.2, abs=0.02)
    aside = watch_at(0.9, 30.0, sonar=0.6).latest(0)  # outside ±12°: not the person's echo
    assert aside is not None and aside.d_m == pytest.approx(0.9 - 0.2, abs=0.03)


def test_an_echo_far_nearer_than_the_camera_is_not_the_person() -> None:
    """A table leg 35 cm ahead, the person seen at 1.24 m (§246): the leg is not the person."""
    e = watch_at(1.24, 0.0, sonar=0.36).latest(0)
    assert e is not None and e.d_m == pytest.approx(1.24 * 0.8, abs=0.03)
    near = watch_at(0.9, 0.0, sonar=0.45).latest(0)  # 45 cm nearer: still the person's arms
    assert near is not None and near.d_m == pytest.approx(0.45 - 0.2, abs=0.02)


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


# ---- final review fixes
def test_a_cut_box_beats_a_far_ranged_one() -> None:  # C1: the baby under the picture, the parent at 2 m
    w = PersonWatch()
    cam = cam_down()
    w.update([box_at(cam, 2.0, 0.0), (200.0, 150.0, 300.0, 239.0)], cam, None, 0)
    e = w.latest(0)
    assert e is not None and e.seen and e.d_m is not None and e.d_m <= 0.35


def test_a_sky_box_does_not_hide_a_cut_one() -> None:  # C1
    w = PersonWatch()
    cam = cam_down()
    w.update([(100.0, 0.0, 140.0, 5.0), (200.0, 150.0, 300.0, 239.0)], cam, None, 0)
    e = w.latest(0)
    assert e is not None and e.seen and e.d_m is not None and e.d_m <= 0.35


def test_a_flickering_near_person_is_not_hidden_by_a_far_one() -> None:  # I1: unsure = nearer
    cam = cam_down()
    w = PersonWatch()
    for i, d in enumerate([0.6, 2.0, 2.0, 0.6, 2.0]):
        w.update([box_at(cam, d, 0.0)], cam, None, i * 60_000)
    e = w.latest(4 * 60_000)
    assert e is not None and e.d_m is not None and e.d_m <= 0.6


def test_the_estimate_carries_the_side_edge() -> None:  # LLD §28.12
    w = PersonWatch()
    cam = cam_down()
    _, top, _, bottom = box_at(cam, 0.8, 0.0)
    e = w.update([(0.0, top, 50.0, bottom)], cam, None, 0)
    assert e is not None and e.edge == 1
    assert watch_at(1.0).latest(0).edge == 0  # type: ignore[union-attr]


def test_a_tank_restart_starts_the_watch_afresh() -> None:  # design-notes §238: the clock went back
    cam = cam_down()
    w = PersonWatch()
    w.update([box_at(cam, 0.3, 0.0)], cam, None, 600 * S)  # handled close before the restart
    e = w.update([box_at(cam, 1.0, -35.0)], cam, None, 5 * S)  # the new clock: sitting 1 m aside
    assert e is not None and e.d_m == pytest.approx(0.80, abs=0.03)
    assert e.coming == 0  # no change of distance across two clocks
    later = w.update([box_at(cam, 1.0, -35.0)], cam, None, 5 * S + 400_000)
    assert later is not None and later.d_m == pytest.approx(0.80, abs=0.03)


def sky_box(cam: CameraModel, bearing_deg: float) -> tuple[float, ...]:
    """A seated person's upper-body box: its bottom above the horizon (no distance), at the bearing."""
    x1, _, x2, _ = box_at(cam, 50.0, bearing_deg)
    box = (x1, 0.0, x2, 60.0)
    assert person_range(cam, box).reason == "sky"
    return box


def test_a_sky_box_has_a_bearing() -> None:  # design-notes §239
    cam = cam_down()
    r = person_range(cam, sky_box(cam, -30.0))
    assert r.dist_m is None and r.bearing_deg == pytest.approx(-30.0, abs=3.0)


def test_a_sky_box_is_seen_at_the_last_measured_distance() -> None:  # §239: owner "A"
    cam = cam_down()
    w = PersonWatch()
    w.update([box_at(cam, 0.9, -30.0)], cam, None, 0)
    e = w.update([sky_box(cam, -30.0)], cam, None, 600_000)
    assert e is not None and e.seen and e.sure == pytest.approx(0.6)
    assert e.d_m == pytest.approx(0.70, abs=0.03) and e.bearing_deg == pytest.approx(-30.0, abs=3.0)


def test_a_sky_box_without_a_recent_distance_reads_close() -> None:  # §239: older than 1 s, or none
    cam = cam_down()
    w = PersonWatch()
    w.update([box_at(cam, 1.0, -30.0)], cam, None, 0)
    e = w.update([sky_box(cam, -30.0)], cam, None, 1_500_000)
    assert e is not None and e.seen and e.d_m == pytest.approx(0.15, abs=0.01)  # 35 cm less 2 sigma
    fresh = PersonWatch().update([sky_box(cam, 10.0)], cam, None, 0)
    assert fresh is not None and fresh.seen and fresh.d_m == pytest.approx(0.15, abs=0.01)


def test_a_sky_box_beside_a_measured_one_takes_its_distance() -> None:  # §239: two boxes, one person
    cam = cam_down()
    e = PersonWatch().update([sky_box(cam, -30.0), box_at(cam, 0.9, -30.0)], cam, None, 0)
    assert e is not None and e.seen and e.d_m == pytest.approx(0.70, abs=0.03)  # not caught
