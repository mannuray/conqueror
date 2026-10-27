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


# ---- final review I1: past the lens's fold there is no answer (not garbage)
def strong_lens(tilt0: float = -20.0) -> Calibration:
    c = calib(tilt0=tilt0, k1=-0.3)
    return Calibration(
        k=np.array([[260.0, 0.0, 160.0], [0.0, 260.0, 120.0], [0.0, 0.0, 1.0]]),
        dist=c.dist,
        size=c.size,
        cam_height_m=c.cam_height_m,
        cam_x_m=c.cam_x_m,
        cam_y_m=c.cam_y_m,
        track_front_m=c.track_front_m,
        tilt0_deg=tilt0,
    )


def test_pixels_beyond_the_lens_fold_have_no_floor_point() -> None:
    cam = at_start(strong_lens())
    assert cam.pixel_to_floor(0, 239) is None  # the corner is past the fold of k1 -0.3 at fx 260
    assert math.isinf(cam.floor_distance(0, 239))
    assert math.isinf(float(cam.expected_floor_depth()[239, 0]))
    p = cam.pixel_to_floor(160, 230)  # well inside: still answered
    assert p is not None


def test_points_far_off_the_axis_are_not_seen() -> None:
    cam = at_start(strong_lens())
    a = math.radians(60)  # 60 deg to the left, 0.6 m away: outside the +-31.6 deg view
    pt = (cam.position()[0] + 0.6 * math.cos(a), 0.6 * math.sin(a))
    assert cam.floor_to_pixel(float(pt[0]), float(pt[1])) is None
    assert not cam.sees(float(pt[0]), float(pt[1]))


def test_the_depth_map_is_cached_per_pan_and_tilt() -> None:
    c = calib(tilt0=-30.0)
    first = at_start(c, 0.0, -5.0).expected_floor_depth()
    again = CameraModel(c).for_frame(Pose(0, 1.0, 2.0, 0.3, 0.0, 0.0), 0.0, -5.0).expected_floor_depth()
    assert again is first  # the depth along each ray depends only on the lens, the mount, pan and tilt
    assert at_start(c, 0.0, 0.0).expected_floor_depth() is not first


# ---- tank session 2026-10-23: the real lens (k3 0.25) left the corners unanswered - a fixed-point
# iteration did not settle there although the lens has no fold
def real_lens() -> Calibration:
    c = calib(tilt0=-0.15)
    return Calibration(
        k=np.array([[182.55, 0.0, 157.42], [0.0, 181.84, 118.26], [0.0, 0.0, 1.0]]),
        dist=np.array([-0.0914, -0.0315, 0.0011, -0.0015, 0.2511]),
        size=c.size,
        cam_height_m=0.178,
        cam_x_m=0.085,
        cam_y_m=0.0,
        track_front_m=0.13,
        tilt0_deg=-0.15,
    )


def test_the_real_lens_answers_every_corner() -> None:
    cal = real_lens()
    cam = CameraModel(cal)
    k = cal.k
    for u, v in [(0, 0), (319, 0), (0, 239), (319, 239)]:
        xd, yd = (u - k[0, 2]) / k[0, 0], (v - k[1, 2]) / k[1, 1]
        n = cam.undistort(xd, yd)
        assert n is not None, (u, v)
        bx, by = cam.distort(*n)
        assert math.isclose(bx, xd, abs_tol=1e-7) and math.isclose(by, yd, abs_tol=1e-7)
    depth = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, -20.0).expected_floor_depth()
    assert math.isfinite(float(depth[239, 0])) and math.isfinite(float(depth[239, 319]))


# ---- the tilt fix from the depth floor (AI engine LLD §18.4)
def test_the_tilt_fix_adds_to_the_real_tilt() -> None:
    c = calib(tilt0=1.0)
    base = CameraModel(c, tilt_deg=-15.0).real_tilt_deg()
    assert math.isclose(CameraModel(c, tilt_deg=-15.0, tilt_fix_deg=2.0).real_tilt_deg(), base + 2.0)


def test_the_floor_depth_cache_tells_tilt_fixes_apart() -> None:
    c = calib()
    a = CameraModel(c, tilt_deg=-15.0).expected_floor_depth()
    b = CameraModel(c, tilt_deg=-15.0, tilt_fix_deg=2.0).expected_floor_depth()
    both = np.isfinite(a) & np.isfinite(b)
    assert both.any() and not np.allclose(a[both], b[both])


def test_the_floor_trick_moves_with_the_tilt_fix() -> None:
    c = calib()
    pose = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)
    plain = CameraModel(c).for_frame(pose, 0.0, -15.0).pixel_to_floor(160.0, 200.0)
    down = CameraModel(c, tilt_fix_deg=-1.0).for_frame(pose, 0.0, -15.0).pixel_to_floor(160.0, 200.0)
    assert plain is not None and down is not None and down[0] < plain[0]  # pointing further down: nearer
