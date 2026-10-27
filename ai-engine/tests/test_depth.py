"""Seeing (AI engine LLD §18.4): the camera's real tilt from the depth floor, metres, heights."""

from __future__ import annotations

import numpy as np
import pytest

from conqueror_ai.domain.camera import CameraModel, FloatArray
from conqueror_ai.domain.depth import AngleFix, CameraAngle, DepthSettings
from conqueror_ai.domain.types import Pose
from tests.test_camera import calib

CAL = calib(k1=-0.05)  # a little barrel distortion, like the real lens
POSE = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)
NO_OFFSET = DepthSettings(tilt_offset_deg=0.0)


def servo_cam(fix: float = 0.0) -> CameraModel:
    """The camera at the driving tilt (-15 deg), with a tilt fix (the truth when fix != 0)."""
    return CameraModel(CAL, tilt_fix_deg=fix).for_frame(POSE, 0.0, -15.0)


def inverse_depth(cam: CameraModel, scale: float = 1.7, shift: float = 0.05) -> FloatArray:
    """What a depth model gives for a bare floor seen by cam: relative inverse depth along the camera's axis
    (an unknown scale and shift); 0 where there is no floor (above the horizon)."""
    z = cam.expected_floor_depth() / cam.ray_norms()
    with np.errstate(divide="ignore", invalid="ignore"):
        inv = np.where(np.isfinite(z) & (z > 0), scale / z + shift, 0.0)
    return inv.astype(np.float64)


def angle_after(true_fix: float, settings: DepthSettings = NO_OFFSET) -> AngleFix:
    a = CameraAngle(settings)
    return a.update(servo_cam(), inverse_depth(servo_cam(true_fix)), 1_000_000)


# ---- CameraAngle
def test_the_depth_floor_finds_a_known_tilt_error() -> None:
    f = angle_after(2.0)
    assert f.trusted and f.tilt_fix_deg == pytest.approx(2.0, abs=0.3)


def test_the_offset_is_taken_off() -> None:
    f = angle_after(4.1, DepthSettings(tilt_offset_deg=4.1))  # the depth reads 4.1 deg less down
    assert f.trusted and f.tilt_fix_deg == pytest.approx(0.0, abs=0.3)


def test_a_fix_over_3_deg_is_not_trusted() -> None:
    f = angle_after(4.0)
    assert not f.trusted and f.tilt_fix_deg == 0.0 and f.depth_tilt_deg is not None


def test_the_median_of_the_last_second_rules() -> None:
    a = CameraAngle(NO_OFFSET)
    f = a.fix()
    for i, fix in enumerate([2.0, 2.0, 10.0, 2.0, 2.0]):
        f = a.update(servo_cam(), inverse_depth(servo_cam(fix)), 1_000_000 + i * 100_000)
    assert f.tilt_fix_deg == pytest.approx(2.0, abs=0.3) and f.n == 5
    f = a.update(servo_cam(), inverse_depth(servo_cam(-1.0)), 2_600_000)  # the others are over 1 s old
    assert f.tilt_fix_deg == pytest.approx(-1.0, abs=0.3) and f.n == 1


def test_no_floor_in_view_keeps_the_last_fix() -> None:
    a = CameraAngle(NO_OFFSET)
    before = a.update(servo_cam(), inverse_depth(servo_cam(1.0)), 1_000_000)
    wall = np.full((240, 320), 0.8)
    after = a.update(servo_cam(), wall, 1_100_000)
    assert after == before


def test_bad_values_are_ignored() -> None:
    inv = inverse_depth(servo_cam(2.0))
    rng = np.random.default_rng(1)
    for bad in (np.nan, np.inf, -1.0):
        idx = rng.integers(0, inv.size, 400)
        inv.flat[idx] = bad
    f = CameraAngle(NO_OFFSET).update(servo_cam(), inv, 1_000_000)
    assert f.tilt_fix_deg == pytest.approx(2.0, abs=0.3)
