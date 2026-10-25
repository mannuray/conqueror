"""The goal: which ball, and where it is (AI engine LLD §17.5)."""

from __future__ import annotations

import pytest

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.drive.goal import Goal, TargetFix, echo_is_target, is_ball, size_position
from conqueror_ai.domain.types import Detection, ObjectEstimate, Pose
from tests.test_camera import calib


# ---- the goal
def obj(oid: int, cls: str, x: float, y: float, seen_us: int = 0) -> ObjectEstimate:
    return ObjectEstimate(oid, cls, x, y, 0.0, 0.0, 0.05, 0, seen_us, "seen")


def test_the_kind_ball() -> None:
    assert is_ball("ball:green") and is_ball("toy") and is_ball("ball")
    assert not is_ball("chair") and not is_ball("person")


def test_the_goal_takes_the_nearest_ball_and_keeps_it() -> None:
    g = Goal()
    fix = g.update(
        [obj(1, "chair", 0.5, 0.0), obj(2, "ball:green", 1.5, 0.0), obj(3, "toy", 2.5, 0.0)], (0, 0), 0
    )
    assert fix is not None and fix.oid == 2
    fix = g.update([obj(2, "ball:green", 1.5, 0.0), obj(4, "ball:red", 0.8, 0.0)], (0, 0), 0)
    assert fix is not None and fix.oid == 2  # locked: a nearer ball appearing doesn't steal it
    fix = g.update([obj(4, "ball:red", 0.8, 0.0)], (0, 0), 0)
    assert fix is not None and fix.oid == 4  # gone: the nearest again


def test_the_goal_uses_the_size_position_and_says_how_old_the_sighting_is() -> None:
    g = Goal()
    fix = g.update(
        [obj(2, "ball:green", 1.5, 0.0, seen_us=1_000_000)], (0, 0), 3_000_000, {"ball:green": (1.1, 0.1)}
    )
    assert fix == TargetFix(2, 1.1, 0.1, 2.0)


def test_a_balls_size_gives_its_range_whatever_the_tilt() -> None:
    cam = CameraModel(calib()).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    r = 200.0 * 0.06 / 1.0  # a 12 cm ball 1 m away at fx 200: radius 12 px
    det = Detection("ball:green", 0.9, (160.0 - r, 120.0 - r, 160.0 + r, 120.0 + r))
    p = size_position(det, 12.0, cam)
    assert p is not None and p[0] == pytest.approx(cam.position()[0] + 1.0, abs=0.01)
    assert (
        size_position(Detection("ball:green", 0.9, (150.0, 110.0, 160.0, 120.0)), 12.0, cam) is None
    )  # too small


def test_an_echo_at_the_targets_range_straight_ahead_is_the_target() -> None:
    assert echo_is_target(0.62, 0.55, 3.0)
    assert not echo_is_target(0.30, 0.55, 3.0)  # nearer: something else
    assert not echo_is_target(0.55, 0.55, 30.0)  # the target is off to the side
