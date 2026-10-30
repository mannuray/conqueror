"""Seeing (AI engine LLD §18.4): the camera's real tilt from the depth floor, metres, heights."""

from __future__ import annotations

import time

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
    """Three pictures (a fix needs MIN_ESTIMATES) of the same floor."""
    a = CameraAngle(settings)
    f = a.fix()
    for i in range(3):
        f = a.update(servo_cam(), inverse_depth(servo_cam(true_fix)), 1_000_000 + i * 100_000)
    return f


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
    for i in range(3):  # the others are over 1 s old
        f = a.update(servo_cam(), inverse_depth(servo_cam(-1.0)), 2_600_000 + i * 100_000)
    assert f.tilt_fix_deg == pytest.approx(-1.0, abs=0.3) and f.n == 3


def test_no_floor_in_view_keeps_the_last_fix() -> None:
    a = CameraAngle(NO_OFFSET)
    before = a.fix()
    for i in range(3):
        before = a.update(servo_cam(), inverse_depth(servo_cam(1.0)), 1_000_000 + i * 100_000)
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
    a = CameraAngle(NO_OFFSET)
    f = a.fix()
    for i in range(3):
        f = a.update(servo_cam(), inv, 1_000_000 + i * 100_000)
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
HP_ANY = HeightPoints(Heights(), Ranges(mark_min_m=0.0))  # heights alone, however near (§171: from 35 cm)


def test_floor_pixels_are_floor() -> None:
    cam = servo_cam()
    rng = scene(cam, [])
    m = HP.classify(cam, rng)
    assert m.floor_xy.shape[0] > 100 and m.low_xy.size == m.high_xy.size == m.above_xy.size == 0
    assert kind_of(m, cam, 160, 200, rng) == "floor"


def test_a_box_gives_low_and_high_points() -> None:
    cam = servo_cam()
    rng = scene(cam, [(160, 120, 0.05), (164, 80, 0.15)])  # 48 and 44 cm from the lens (from 35 cm, §171)
    m = HP.classify(cam, rng)
    assert kind_of(m, cam, 160, 120, rng) == "low" and kind_of(m, cam, 164, 80, rng) == "high"


def test_an_18_cm_board_is_high_and_a_30_cm_one_is_above() -> None:
    cam = servo_cam()
    # both are above the lens (17.8 cm): pixels looking a little up see them (the 18 cm one only 6.5 cm off -
    # no sampled pixel puts it past 35 cm, so heights alone)
    rng = scene(cam, [(160, 60, 0.18), (160, 8, 0.30)])
    m = HP_ANY.classify(cam, rng)
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


def test_obstacles_are_marked_only_to_1_5_m() -> None:  # design-notes §169 A: farther, ~13 % too near
    cam = servo_cam()
    # a point 3 cm up on a floor ray at D sits at D x (1 - 3 / 17.8): ~1.05 m for D ~1.26, ~1.78 m for D ~2.14
    near = [(160, v) for v in range(0, 240, 4) if 1.2 < cam.floor_distance(160, v) < 1.3]
    far = [(160, v) for v in range(0, 240, 4) if 2.0 < cam.floor_distance(160, v) < 2.3]
    assert near and far
    rng = scene(cam, [(*near[0], 0.03), (*far[0], 0.03)])
    m = HP.classify(cam, rng)
    assert kind_of(m, cam, *near[0], rng) == "low"
    assert kind_of(m, cam, *far[0], rng) is None
    bare = scene(cam, [])
    assert kind_of(HP.classify(cam, bare), cam, *far[0], bare) == "floor"  # its floor still seen


def test_obstacles_are_marked_only_from_35_cm() -> None:  # design-notes §171 B: the picture's bottom edge
    """Floor run 3: the floor's dark stripe read as raised at the picture's bottom edge, ~25 cm from the
    camera, and froze the tank. Nearer than 35 cm nothing is marked an obstacle; the floor there is still
    seen."""
    cam = servo_cam()
    # a point 3 cm up on a floor ray at D sits at D x (1 - 3 / 17.8): ~0.28 m for D ~0.33, ~0.50 m for D ~0.6
    near = [(160, v) for v in range(0, 240, 4) if 0.30 < cam.floor_distance(160, v) < 0.36]
    mid = [(160, v) for v in range(0, 240, 4) if 0.55 < cam.floor_distance(160, v) < 0.65]
    assert near and mid
    rng = scene(cam, [(*near[0], 0.03), (*mid[0], 0.03)])
    m = HP.classify(cam, rng)
    assert kind_of(m, cam, *near[0], rng) is None
    assert kind_of(m, cam, *mid[0], rng) == "low"
    bare = scene(cam, [])
    assert kind_of(HP.classify(cam, bare), cam, *near[0], bare) == "floor"


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
    rng = scene(cam, [(160, 44, 0.22)])  # above the lens: a pixel looking up, 40 cm off
    owners = HeightPoints(Heights(clear_m=0.25), Ranges())
    assert kind_of(owners.classify(cam, rng), cam, 160, 44, rng) == "high"
    assert kind_of(HP.classify(cam, rng), cam, 160, 44, rng) == "above"


# ---- Seeing: one picture end to end
def test_a_synthetic_scene_comes_out_right() -> None:
    true = servo_cam(1.5)
    box = [(u, v, 0.05) for u in range(148, 176, 4) for v in range(120, 140, 4)]  # 37-53 cm off (§171)
    rng = scene(true, box)
    with np.errstate(divide="ignore", invalid="ignore"):
        z = rng / true.ray_norms()
        inv = np.where(np.isfinite(z) & (z > 0), 1.7 / z + 0.05, 0.0)
    s = Seeing(MapConfig(depth=NO_OFFSET))
    seen = s.see(servo_cam(), inv, 1_000_000)
    for i in range(1, 3):
        seen = s.see(servo_cam(), inv, 1_000_000 + i * 100_000)
    assert seen.fix.trusted and seen.fix.tilt_fix_deg == pytest.approx(1.5, abs=0.3)
    assert seen.cam.tilt_fix_deg == seen.fix.tilt_fix_deg and seen.marks is not None
    truth = HP.classify(true, rng).low_xy
    assert seen.marks.low_xy.shape[0] >= 0.8 * truth.shape[0] > 0
    for x, y in seen.marks.low_xy:
        assert np.min(np.hypot(truth[:, 0] - x, truth[:, 1] - y)) < 0.05


def test_a_wall_close_up_gives_no_marks_and_keeps_the_fix() -> None:
    s = Seeing(MapConfig(depth=NO_OFFSET))
    for i in range(3):
        s.see(servo_cam(), inverse_depth(servo_cam(1.0)), 1_000_000 + i * 100_000)
    seen = s.see(servo_cam(), np.full((240, 320), 0.8), 1_400_000)
    assert seen.marks is None and seen.scaled is None
    assert seen.fix.tilt_fix_deg == pytest.approx(1.0, abs=0.3)


# ---- final review fixes
def test_one_picture_is_not_enough_for_a_fix() -> None:  # M1
    f = CameraAngle(NO_OFFSET).update(servo_cam(), inverse_depth(servo_cam(2.0)), 1_000_000)
    assert f.fresh and f.tilt_fix_deg == 0.0 and f.n == 1


def test_the_fix_comes_in_steps_of_0_05_deg() -> None:  # M5: the floor-depth cache can hit
    f = angle_after(1.37)
    assert abs(f.tilt_fix_deg / 0.05 - round(f.tilt_fix_deg / 0.05)) < 1e-9


def vga_cam() -> CameraModel:
    """The tank's real lens and mount, at VGA (as cq-drive runs)."""
    from conqueror_ai.adapters import calibration_store
    from conqueror_ai.app.objects import CALIBRATION, VGA

    return CameraModel(calibration_store.load(CALIBRATION, VGA)).for_frame(POSE, 0.0, -15.0)


def test_seeing_a_vga_picture_is_quick() -> None:  # C1: it runs every picture, beside the driving loop
    cam = vga_cam()
    inv = inverse_depth(cam)
    s = Seeing(MapConfig(depth=NO_OFFSET))
    s.see(cam, inv, 1_000_000)  # the caches warm
    times: list[float] = []
    for i in range(5):  # the fastest of five: a busy laptop doesn't fail it
        t = time.perf_counter()
        s.see(cam, inv, 1_100_000 + i * 100_000)
        times.append(time.perf_counter() - t)
    assert min(times) < 0.06
