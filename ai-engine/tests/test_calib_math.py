"""The calibration maths (AI engine LLD §8, design-notes §123): the laser horizon, finding lines, servo map,
the floor-mark check and the tilt study - on synthetic pictures and a synthetic camera."""

from __future__ import annotations

import math

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain import calib_math as cm
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Pose
from tests.test_camera import calib


def red_line_picture(row: float | None = None, col: float | None = None) -> NDArray[np.uint8]:
    """A grey 320 x 240 RGB picture with a 3-pixel-wide red laser line (blurred edges) at row or col."""
    img = np.full((240, 320, 3), 90, dtype=np.uint8)
    rng = np.random.default_rng(1)
    img = np.clip(img + rng.normal(0, 8, img.shape), 0, 255).astype(np.uint8)
    if row is not None:
        for r in range(240):
            w = max(0.0, 1.0 - abs(r - row) / 2.0)
            img[r, :, 0] = np.clip(img[r, :, 0] + 160 * w, 0, 255)
    if col is not None:
        for c in range(320):
            w = max(0.0, 1.0 - abs(c - col) / 2.0)
            img[:, c, 0] = np.clip(img[:, c, 0] + 160 * w, 0, 255)
    return img


def test_the_laser_line_is_found_to_a_fraction_of_a_pixel() -> None:
    r = cm.laser_row(red_line_picture(row=87.4))
    assert r is not None and abs(r - 87.4) < 0.3
    c = cm.laser_col(red_line_picture(col=201.6))
    assert c is not None and abs(c - 201.6) < 0.3


def test_no_line_no_answer() -> None:
    assert cm.laser_row(red_line_picture()) is None
    assert cm.laser_col(red_line_picture()) is None


def test_the_horizon_row_gives_the_tilt() -> None:
    c = calib()  # fy 200, cy 120, no distortion
    assert math.isclose(cm.horizon_tilt_deg(c, 120.0), 0.0, abs_tol=1e-9)
    assert math.isclose(cm.horizon_tilt_deg(c, 120.0 - 200.0 * math.tan(math.radians(7))), -7.0, abs_tol=1e-6)


def test_the_horizon_row_matches_the_camera_model() -> None:  # a level line at lens height through the model
    c = calib(tilt0=-9.0, k1=-0.1)
    cam = CameraModel(c).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    right, down, fwd = cam.axes()
    level = np.array([1.0, 0.0, 0.0])  # straight ahead, level
    _xd, yd = cam.distort(float(level @ right) / float(level @ fwd), float(level @ down) / float(level @ fwd))
    row = c.k[1, 1] * yd + c.k[1, 2]
    assert math.isclose(cm.horizon_tilt_deg(c, row), -9.0, abs_tol=1e-6)


def test_a_vertical_line_straight_ahead_gives_the_pan() -> None:
    c = calib()
    col = 160.0 - 200.0 * math.tan(math.radians(12))  # pan 12 deg right: the line moves left
    assert math.isclose(cm.pan_from_col_deg(c, col), 12.0, abs_tol=1e-6)


def test_the_servo_map_is_a_straight_line_fit() -> None:
    gain, offset = cm.fit_linear([-15.0, 0.0, 15.0, 30.0], [-14.0, 0.4, 14.8, 29.2])
    assert math.isclose(gain, 0.968, abs_tol=0.01) and math.isclose(offset, 0.36, abs_tol=0.05)


def test_the_floor_mark_check() -> None:
    c = calib(tilt0=-30.0)
    cam = CameraModel(c).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    q = cam.floor_to_pixel(c.track_front_m + 0.50, 0.0)
    assert q is not None
    err = cm.floor_mark_error_m(cam, q[0], q[1], 0.50)
    assert err is not None and err < 1e-6
    q2 = cam.floor_to_pixel(c.track_front_m + 0.53, 0.0)  # a mark 3 cm further than its label
    assert q2 is not None
    err2 = cm.floor_mark_error_m(cam, q2[0], q2[1], 0.50)
    assert err2 is not None and math.isclose(err2, 0.03, abs_tol=1e-6)


def test_the_tilt_study_shrinks_the_blind_zone_as_the_camera_looks_down() -> None:
    rows = cm.tilt_study(calib(tilt0=-7.0), [0.0, -8.0, -18.0])
    assert [r.tilt_deg for r in rows] == [0.0, -8.0, -18.0]
    assert rows[0].blind_m > rows[1].blind_m > rows[2].blind_m > 0
    assert rows[0].horizon_in_view and rows[1].horizon_in_view  # walls still in view
    for r in rows:
        assert r.far_m > r.blind_m
