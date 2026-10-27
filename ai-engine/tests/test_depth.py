"""Seeing (AI engine LLD §18.4): the camera's real tilt from the depth floor, metres, heights."""

from __future__ import annotations

import numpy as np
import pytest

from conqueror_ai.domain.camera import CameraModel, FloatArray
from conqueror_ai.domain.depth import (
    AngleFix,
    CameraAngle,
    DepthScaler,
    DepthSettings,
    HeightPoints,
    Heights,
    MapConfig,
    Marks,
    Ranges,
    Seeing,
)
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
    assert before.fresh and not after.fresh
    assert (after.tilt_fix_deg, after.trusted) == (before.tilt_fix_deg, before.trusted)


def test_bad_values_are_ignored() -> None:
    inv = inverse_depth(servo_cam(2.0))
    rng = np.random.default_rng(1)
    for bad in (np.nan, np.inf, -1.0):
        idx = rng.integers(0, inv.size, 400)
        inv.flat[idx] = bad
    f = CameraAngle(NO_OFFSET).update(servo_cam(), inv, 1_000_000)
    assert f.tilt_fix_deg == pytest.approx(2.0, abs=0.3)


# ---- DepthScaler
def test_the_floor_scale_is_recovered() -> None:
    cam = servo_cam()
    sd = DepthScaler().fit(cam, inverse_depth(cam))
    assert sd is not None
    floor = cam.floor_mask()
    assert np.allclose(sd.range_m[floor], cam.expected_floor_depth()[floor], rtol=0.01)
    assert sd.a == pytest.approx(1 / 1.7, rel=0.01) and sd.c == pytest.approx(-0.05 / 1.7, abs=1e-3)


def test_too_little_floor_gives_none() -> None:
    cam = servo_cam()
    inv = inverse_depth(cam)
    keep = np.argwhere(cam.floor_mask())[:400]
    thin = np.zeros_like(inv)
    thin[keep[:, 0], keep[:, 1]] = inv[keep[:, 0], keep[:, 1]]
    assert DepthScaler().fit(cam, thin) is None


def test_a_wrong_way_fit_gives_none() -> None:
    cam = servo_cam()
    inv = inverse_depth(cam)
    assert DepthScaler().fit(cam, np.where(inv > 0, 10.0 - inv, 0.0)) is None  # nearer reads smaller: a <= 0


def test_a_flat_depth_picture_gives_none() -> None:
    assert DepthScaler().fit(servo_cam(), np.full((240, 320), 0.8)) is None  # a wall: nothing to fit on


def test_ranges_where_the_fit_goes_negative_are_inf_not_negative() -> None:
    cam = servo_cam()
    inv = inverse_depth(cam)
    inv[:5, :] = 0.001  # far above the floor's values: 1 / (a inv + c) would be negative
    sd = DepthScaler().fit(cam, inv)
    assert sd is not None and np.all(np.isinf(sd.range_m[:5, :]) | (sd.range_m[:5, :] > 0))
    assert not np.any(sd.range_m < 0)


def test_bad_inverse_depth_values_are_left_out() -> None:
    cam = servo_cam()
    inv = inverse_depth(cam)
    floor = np.argwhere(cam.floor_mask())
    for i, bad in enumerate((np.nan, np.inf, -1.0, 0.0)):
        r, c = floor[100 + i]
        inv[r, c] = bad
    sd = DepthScaler().fit(cam, inv)
    assert sd is not None and sd.a == pytest.approx(1 / 1.7, rel=0.01)
    for i in range(4):
        r, c = floor[100 + i]
        assert np.isinf(sd.range_m[r, c])


# ---- HeightPoints
def range_for_height(cam: CameraModel, u: int, v: int, z: float) -> float:
    """The range along pixel (u, v)'s ray where it reaches height z (inf if it never does)."""
    ray = cam.pixel_ray(u, v)
    assert ray is not None
    unit = ray / np.linalg.norm(ray)
    t = (z - cam.position()[2]) / unit[2] if unit[2] != 0 else -1.0
    return t if t > 0 else float("inf")


def scene(cam: CameraModel, patches: list[tuple[int, int, float]]) -> FloatArray:
    """The bare floor's ranges, with the given pixels (u, v) set to reach height z."""
    rng = cam.expected_floor_depth().copy()
    for u, v, z in patches:
        rng[v, u] = range_for_height(cam, u, v, z)
    return rng


def kind_of(marks: Marks, cam: CameraModel, u: int, v: int, rng: FloatArray) -> str | None:
    """Which class of marks holds the point pixel (u, v) makes."""
    ray = cam.pixel_ray(u, v)
    assert ray is not None
    p = cam.position() + ray / np.linalg.norm(ray) * rng[v, u]
    for name in ("floor", "low", "high", "above"):
        xy: FloatArray = getattr(marks, f"{name}_xy")
        if xy.size and np.min(np.hypot(xy[:, 0] - p[0], xy[:, 1] - p[1])) < 1e-6:
            return name
    return None


HP = HeightPoints(Heights(), Ranges())


def test_floor_pixels_are_floor() -> None:
    cam = servo_cam()
    rng = scene(cam, [])
    m = HP.classify(cam, rng)
    assert m.floor_xy.shape[0] > 100 and m.low_xy.size == m.high_xy.size == m.above_xy.size == 0
    assert kind_of(m, cam, 160, 200, rng) == "floor"


def test_a_box_gives_low_and_high_points() -> None:
    cam = servo_cam()
    rng = scene(cam, [(160, 180, 0.05), (164, 180, 0.15)])
    m = HP.classify(cam, rng)
    assert kind_of(m, cam, 160, 180, rng) == "low" and kind_of(m, cam, 164, 180, rng) == "high"


def test_an_18_cm_board_is_high_and_a_30_cm_one_is_above() -> None:
    cam = servo_cam()
    # both are above the lens (17.8 cm): pixels looking a little up see them
    rng = scene(cam, [(160, 60, 0.18), (160, 8, 0.30)])
    m = HP.classify(cam, rng)
    assert kind_of(m, cam, 160, 60, rng) == "high" and kind_of(m, cam, 160, 8, rng) == "above"


def test_obstacles_beyond_2_m_are_not_marked_floor_to_3_m() -> None:
    cam = servo_cam()
    # a point 3 cm up on a floor ray at ~2.8 m sits at ~2.3 m: beyond the 2 m for obstacles
    far = [(u, v) for v in range(0, 240, 4) for u in (160,) if 2.6 < cam.floor_distance(u, v) < 2.9]
    assert far
    u, v = far[0]
    rng = scene(cam, [])
    assert kind_of(HP.classify(cam, rng), cam, u, v, rng) == "floor"
    rng = scene(cam, [(u, v, 0.03)])
    assert kind_of(HP.classify(cam, rng), cam, u, v, rng) is None


def test_points_under_the_floor_and_bad_ranges_are_dropped() -> None:
    cam = servo_cam()
    rng = scene(cam, [(160, 200, -0.10)])
    rng[204, 160], rng[208, 160] = np.inf, np.nan
    m = HP.classify(cam, rng)
    assert kind_of(m, cam, 160, 200, rng) is None
    total = sum(getattr(m, f"{k}_xy").shape[0] for k in ("floor", "low", "high", "above"))
    assert total == HP.classify(cam, scene(cam, [])).floor_xy.shape[0] - 3


def test_the_owners_clear_height_is_used() -> None:
    cam = servo_cam()
    rng = scene(cam, [(160, 8, 0.22)])  # above the lens: a pixel looking up
    owners = HeightPoints(Heights(clear_m=0.25), Ranges())
    assert kind_of(owners.classify(cam, rng), cam, 160, 8, rng) == "high"
    assert kind_of(HP.classify(cam, rng), cam, 160, 8, rng) == "above"


# ---- Seeing: one picture end to end
def test_a_synthetic_scene_comes_out_right() -> None:
    true = servo_cam(1.5)
    box = [(u, v, 0.05) for u in range(148, 176, 4) for v in range(172, 196, 4)]
    rng = scene(true, box)
    with np.errstate(divide="ignore", invalid="ignore"):
        z = rng / true.ray_norms()
        inv = np.where(np.isfinite(z) & (z > 0), 1.7 / z + 0.05, 0.0)
    seen = Seeing(MapConfig(depth=NO_OFFSET)).see(servo_cam(), inv, 1_000_000)
    assert seen.fix.trusted and seen.fix.tilt_fix_deg == pytest.approx(1.5, abs=0.3)
    assert seen.cam.tilt_fix_deg == seen.fix.tilt_fix_deg and seen.marks is not None
    truth = HP.classify(true, rng).low_xy
    assert seen.marks.low_xy.shape[0] >= 0.8 * truth.shape[0] > 0
    for x, y in seen.marks.low_xy:
        assert np.min(np.hypot(truth[:, 0] - x, truth[:, 1] - y)) < 0.05


def test_a_wall_close_up_gives_no_marks_and_keeps_the_fix() -> None:
    s = Seeing(MapConfig(depth=NO_OFFSET))
    s.see(servo_cam(), inverse_depth(servo_cam(1.0)), 1_000_000)
    seen = s.see(servo_cam(), np.full((240, 320), 0.8), 1_100_000)
    assert seen.marks is None and seen.scaled is None
    assert seen.fix.tilt_fix_deg == pytest.approx(1.0, abs=0.3)
