"""Spec 10 phase 0 (Plan P0): a crawling person's distance and direction from a box, against the tape."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.person_test import (
    Mark,
    PersonRange,
    person_range,
    pick_person,
    score_mark,
    verdict,
)
from tests.test_camera import at_start, calib

FRONT = 0.10  # the test calibration's tracks' front (track_front_m)


def cam_down() -> CameraModel:
    return at_start(calib(tilt0=-15.0))  # the driving tilt


def box_at(
    cam: CameraModel, d: float, bearing_deg: float, w: float = 60.0, h: float = 80.0
) -> tuple[float, ...]:
    """A person-sized box whose bottom centre is the floor point d from the front edge at the bearing."""
    a = math.radians(bearing_deg)
    px = cam.floor_to_pixel(FRONT + d * math.cos(a), d * math.sin(a))
    assert px is not None
    u, v = px
    return (u - w / 2, v - h, u + w / 2, v)


def test_a_box_on_the_floor_1_m_ahead_reads_1_m_straight_ahead() -> None:
    cam = cam_down()
    r = person_range(cam, box_at(cam, 1.0, 0.0))
    assert (
        r.reason == ""
        and r.dist_m == pytest.approx(1.0, abs=0.01)
        and r.bearing_deg == pytest.approx(0.0, abs=0.5)
    )


def test_30_degrees_right_reads_minus_30() -> None:
    cam = cam_down()
    r = person_range(cam, box_at(cam, 0.8, -30.0))
    assert r.dist_m == pytest.approx(0.8, abs=0.01) and r.bearing_deg == pytest.approx(-30.0, abs=1.0)


def test_a_box_cut_by_the_pictures_bottom_is_not_a_distance() -> None:
    r = person_range(cam_down(), (100.0, 150.0, 200.0, 239.6))
    assert r.reason == "cut" and r.dist_m is None


def test_a_box_whose_bottom_is_above_the_horizon_is_not_a_distance() -> None:
    r = person_range(
        at_start(calib()), (100.0, 20.0, 200.0, 100.0)
    )  # level camera: the centre row is the horizon
    assert r.reason == "sky" and r.dist_m is None


def test_of_two_people_the_nearer_on_the_floor_is_the_one() -> None:
    cam = cam_down()
    far, near = person_range(cam, box_at(cam, 1.5, 0.0)), person_range(cam, box_at(cam, 0.7, 10.0))
    cut = person_range(cam, (0.0, 100.0, 50.0, 240.0))
    assert pick_person([far, cut, near]) == near
    assert pick_person([cut]) == cut  # seen, though not ranged
    assert pick_person([]) is None


def seen(d: float, bearing: float = 0.0) -> PersonRange:
    return PersonRange(d, bearing, "")


def test_1_m_still_passes_with_9_seen_and_8_within_10_cm() -> None:
    m = Mark("1.0-0-still-15", 1.0, 0.0, still=True, tilt_deg=-15.0)
    pics = [seen(1.05)] * 6 + [seen(0.93)] * 2 + [seen(1.2)] + [None]
    s = score_mark(m, pics, [None] * 10)
    assert (s.seen, s.pictures, s.ranged_ok, s.dir_ok) == (9, 10, 8, 9)
    assert s.passed


def test_7_of_10_seen_fails() -> None:
    m = Mark("1.0-0-still-15", 1.0, 0.0, still=True, tilt_deg=-15.0)
    s = score_mark(m, [seen(1.0)] * 7 + [None] * 3, [None] * 10)
    assert not s.passed and "seen 7/10" in s.why


def test_beyond_1_m_the_distance_may_be_20_percent_out() -> None:
    m = Mark("1.5-0-still-15", 1.5, 0.0, still=True, tilt_deg=-15.0)
    assert score_mark(m, [seen(1.75)] * 10, []).passed  # 17 % out
    assert not score_mark(m, [seen(1.85)] * 10, []).passed  # 23 % out


def test_the_direction_must_be_within_5_degrees() -> None:
    m = Mark("1.0-30-still-15", 1.0, -30.0, still=True, tilt_deg=-15.0)
    assert score_mark(m, [seen(1.0, -33.0)] * 10, []).passed
    s = score_mark(m, [seen(1.0, -37.0)] * 10, [])
    assert not s.passed and "direction" in s.why


def test_near_and_ahead_the_sonar_must_see_them() -> None:
    m = Mark("0.5-0-still-15", 0.5, 0.0, still=True, tilt_deg=-15.0)
    ok = score_mark(m, [seen(0.5)] * 10, [520] * 8 + [None, 900])
    assert ok.sonar_ok == 8 and ok.passed
    bad = score_mark(m, [seen(0.5)] * 10, [700] * 10)
    assert not bad.passed and "sonar" in bad.why


def test_30_cm_is_reported_not_scored_for_the_camera() -> None:
    m = Mark("0.3-0-still-15", 0.3, 0.0, still=True, tilt_deg=-15.0)
    s = score_mark(m, [None] * 10, [300] * 10)  # the camera sees nothing that close: the sonar's case
    assert s.passed and s.camera_scored is False


def test_the_verdict_lists_every_mark_and_the_overall_answer() -> None:
    m1 = Mark("1.0-0-still-15", 1.0, 0.0, still=True, tilt_deg=-15.0)
    m2 = Mark("2.0-0-still-15", 2.0, 0.0, still=True, tilt_deg=-15.0)
    good, bad = score_mark(m1, [seen(1.0)] * 10, []), score_mark(m2, [None] * 10, [])
    text = verdict("yolo person", [good, bad])
    assert "1.0-0-still-15" in text and "2.0-0-still-15" in text
    assert text.splitlines()[-1] == "yolo person: FAIL (1 of 2 marks pass)"
    assert verdict("yolo person", [good]).splitlines()[-1] == "yolo person: PASS (1 of 1 marks pass)"


def test_a_crawling_mark_is_graded_on_seen_and_distance_only() -> None:
    """Review I2: crawling across the mark moves the person sideways - direction and sonar reported only."""
    m = Mark("0.5-0-crawl-t-15", 0.5, 0.0, still=False, tilt_deg=-15.0)
    s = score_mark(m, [seen(0.5, 15.0)] * 10, [900] * 10)
    assert s.passed and s.dir_ok == 0 and s.sonar_ok == 0
    assert not score_mark(m, [seen(0.8, 0.0)] * 10, []).passed  # the distance still counts


def test_a_box_a_few_pixels_above_the_bottom_is_cut() -> None:
    """Review minor: a box clipped near the edge reads as a range near the picture's bottom."""
    assert person_range(cam_down(), (100.0, 150.0, 200.0, 237.0)).reason == "cut"


def test_a_box_touching_a_side_edge_says_which() -> None:  # AI engine LLD §28.12: at least that far round
    cam = cam_down()
    w = cam.cal.size[0]
    _, top, _, bottom = box_at(cam, 0.8, 0.0)
    assert person_range(cam, (0.0, top, 50.0, bottom)).edge == 1  # the picture's left: + left
    assert person_range(cam, (w - 50.0, top, float(w), bottom)).edge == -1
    assert person_range(cam, box_at(cam, 0.8, 0.0)).edge == 0
