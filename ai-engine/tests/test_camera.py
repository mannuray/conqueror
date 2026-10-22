"""CameraModel (AI engine LLD §5.3): pixel <-> floor through the lens, the mount and the pan / tilt servos."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose


def calib(tilt0: float = 0.0, k1: float = 0.0) -> Calibration:
    return Calibration(
        k=np.array([[200.0, 0.0, 160.0], [0.0, 200.0, 120.0], [0.0, 0.0, 1.0]]),
        dist=np.array([k1, 0.0, 0.0, 0.0, 0.0]),
        size=(320, 240),
        cam_height_m=0.178,
        cam_x_m=0.05,
        cam_y_m=0.0,
        track_front_m=0.10,
        tilt0_deg=tilt0,
    )


def at_start(c: Calibration, pan: float = 0.0, tilt: float = 0.0) -> CameraModel:
    return CameraModel(c).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), pan, tilt)


def test_a_level_camera_never_sees_the_floor_above_the_centre_row() -> None:
    cam = at_start(calib())
    assert cam.pixel_to_floor(160, 120) is None  # the centre ray is level
    assert cam.pixel_to_floor(160, 60) is None


def test_the_floor_under_a_downward_ray_is_where_trigonometry_says() -> None:
    cam = at_start(calib(tilt0=-30.0))  # looking 30 deg down
    p = cam.pixel_to_floor(160, 120)  # the centre ray hits the floor at h / tan 30 ahead of the lens
    assert p is not None
    assert math.isclose(p[0], 0.05 + 0.178 / math.tan(math.radians(30)), abs_tol=1e-6)
    assert math.isclose(p[1], 0.0, abs_tol=1e-9)


def test_pixel_floor_round_trips_with_distortion_pan_and_tilt() -> None:
    cam = at_start(calib(tilt0=-20.0, k1=-0.1), pan=15.0, tilt=-5.0)  # a lens like the OV3660 (barrel)
    for u, v in [(40, 200), (160, 180), (300, 230), (100, 150)]:
        p = cam.pixel_to_floor(u, v)
        assert p is not None
        q = cam.floor_to_pixel(*p)
        assert q is not None
        assert math.isclose(q[0], u, abs_tol=0.05) and math.isclose(q[1], v, abs_tol=0.05)


def test_pan_right_looks_to_the_right() -> None:
    cam = at_start(calib(tilt0=-30.0), pan=20.0)  # pan + = right; the room's y is left
    p = cam.pixel_to_floor(160, 120)
    assert p is not None and p[1] < 0
    assert math.isclose(math.degrees(math.atan2(-p[1], p[0] - 0.05)), 20.0, abs_tol=1e-6)


def test_the_tank_pose_moves_the_floor_point() -> None:
    c = calib(tilt0=-30.0)
    ahead = CameraModel(c).for_frame(Pose(0, 1.0, 2.0, math.pi / 2, 0.0, 0.0), 0.0, 0.0)  # facing +y
    p = ahead.pixel_to_floor(160, 120)
    assert p is not None
    d = 0.05 + 0.178 / math.tan(math.radians(30))
    assert math.isclose(p[0], 1.0, abs_tol=1e-6) and math.isclose(p[1], 2.0 + d, abs_tol=1e-6)


def test_expected_floor_depth_and_the_trusted_mask() -> None:
    cam = at_start(calib(tilt0=-30.0))
    depth = cam.expected_floor_depth()
    assert depth.shape == (240, 320)
    assert math.isinf(depth[0, 160])  # the top row looks above the horizon? (30 deg down, 31 deg half view)
    centre = 0.178 / math.sin(math.radians(30))
    assert math.isclose(float(depth[120, 160]), centre, rel_tol=1e-6)
    mask = cam.floor_mask()
    assert mask[120, 160] and not mask[0, 160]


def test_sees_only_floor_points_in_the_picture_and_near_enough() -> None:
    cam = at_start(calib(tilt0=-30.0))
    assert cam.sees(0.5, 0.0)
    assert not cam.sees(-0.5, 0.0)  # behind
    assert not cam.sees(0.5, 2.0)  # far to the side
    assert not cam.sees(4.0, 0.0)  # beyond 2.5 m


def test_servo_angles_from_telemetry_become_degrees_from_the_centre() -> None:
    from conqueror_ai.domain.camera import from_servo

    assert from_servo(90.0, 90.0) == (0.0, 0.0)
    assert from_servo(75.0, 105.0) == (15.0, -15.0)  # right / up are the smaller servo angles (LOOK, cam_set)
